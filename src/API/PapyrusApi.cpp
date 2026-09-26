#include "API/PapyrusApi.h"

#include "API/BridgeApi.h"
#include "Core/StringUtil.h"  // ToLowerAscii
#include "Core/Version.h"
#include "Core/Ids.h"  // opaque id safety validation + case-insensitive matching

#include <atomic>

#include "RE/B/BSScriptUtil.h"       // BindNativeMethod marshaling, GameVM, VirtualMachine
#include "RE/E/Events.h"             // TESLoadGameEvent
#include "RE/F/FORM_ENUM_STRING.h"   // FormType -> record-signature table
#include "RE/RTTI.h"                 // starfield_cast (TESForm -> TESFullName)
#include "RE/T/TESForm.h"            // LookupByID + form identity reads
#include "RE/T/TESFullName.h"        // display-name component

namespace OSFUI::API::Papyrus
{
	namespace
	{
		using PapVM = RE::BSScript::IVirtualMachine;
		using VM = RE::BSScript::Internal::VirtualMachine;

		enum class Kind : std::uint8_t
		{
			kSend,
			kRequest,
		};

		// Weak receiver identity: a VM object handle plus its lower-cased script name, never a VM pointer or
		// refcount. A zero handle selects a GLOBAL function on the script. The VM resolves the identity at
		// dispatch time, so a receiver from a dead session simply fails to dispatch instead of being kept alive.
		struct Receiver
		{
			std::uint64_t handle{ 0 };
			std::string   script;
			bool          operator==(const Receiver&) const = default;
		};

		// Registrations live until the next game load or main-menu return; there is no per-entry removal.
		struct Entry
		{
			Kind              kind{ Kind::kSend };
			Receiver          receiver;
			RE::BSFixedString fn;
			std::string       modId;
			std::string       key;  // exact view endpoint
		};

		// Queue FormIDs, never TESForm pointers; serialize them on the main thread.
		struct QueuedState
		{
			std::string                               mod;
			std::string                               key;
			nlohmann::json                            value;
			std::optional<std::uint32_t>              formId;
			std::optional<std::vector<std::uint32_t>> formIds;
		};

		// Keep portable values/FormIDs only; serialize forms on the main thread.
		struct QueuedEvent
		{
			std::string        mod;
			std::string        name;
			std::vector<Value> args;
		};

		struct PendingViewRequest
		{
			std::string                               token;
			std::string                               view;
			std::string                               deferToken;
			bool                                      answered{ false };
			bool                                      rejected{ false };
			std::string                               code;
			std::string                               message;
			nlohmann::json                            value;
			std::optional<std::uint32_t>              formId;
			std::optional<std::vector<std::uint32_t>> formIds;
		};
		// Leak this VM-owned state intentionally because process-detach destruction is unsafe.
		struct ProcessState
		{
			std::mutex                                          lock;
			std::atomic_bool                                   pending{ false };
			std::vector<Entry>                                  entries;
			std::vector<QueuedState>                            states;
			std::vector<QueuedEvent>                            events;
			std::unordered_map<std::string, PendingViewRequest> viewRequests;
			std::uint64_t                                       nextViewRequest{ 1 };
			// Raised on game load so Runtime::Update purges session-scoped retained state.
			bool                                                sessionReset{ false };
			// Set while a world-replacing save/load operation is in flight; dispatch into the VM is refused until
			// the new session is announced (TESLoadGameEvent) or the operation fails.
			std::atomic_bool                                    suspended{ false };
			std::atomic_bool                                    nativesBound{ false };
		};

		ProcessState& State()
		{
			static ProcessState* const state = new ProcessState;
			return *state;
		}

		void MarkPending() noexcept
		{
			State().pending.store(true, std::memory_order_release);
		}

		// BSFixedString preserves process-first casing, so normalize before matching.
		using StringUtil::ToLowerAscii;

		// Registration results returned to Papyrus; documented in OSFUI.psc.
		enum RegisterResult : std::int32_t
		{
			kRegistered = 1,
			kErrInvalidTarget = -1,  // None receiver or empty script name
			kErrInvalidModId = -2,
			kErrInvalidName = -3,    // reserved, malformed, or qualified name too long
			kErrConflict = -4,       // endpoint owned by another script or registered with the other kind
			kErrTableFull = -5,
		};

		constexpr std::size_t kMaxEntries = 0xFFFF;

		// Caller holds the process-state lock.
		std::int32_t AddEntry(Kind a_kind, Receiver a_receiver, std::string_view a_fn, std::string_view a_modId, std::string_view a_key)
		{
			if (State().entries.size() >= kMaxEntries) {
				REX::ERROR("PapyrusApi: callback table full");
				return kErrTableFull;
			}

			Entry& e = State().entries.emplace_back();
			e.kind = a_kind;
			e.receiver = std::move(a_receiver);
			e.fn = RE::BSFixedString(std::string(a_fn).c_str());
			e.modId = std::string(a_modId);
			e.key = std::string(a_key);

			const char* signature = a_kind == Kind::kRequest ? "string, Var[], string, string" : "string, Var[], string";
			REX::DEBUG("PapyrusApi: registered {}.{}({}) on {} ({} filter '{}'{}{})",
				e.receiver.script, e.fn.c_str(), signature,
				e.receiver.handle ? "instance" : "global",
				a_kind == Kind::kSend ? "send" : "request",
				e.modId, e.key.empty() ? "" : ".", e.key);
			return kRegistered;
		}

		void PackValue(RE::BSScript::Variable& a_out, const Value& a_value)
		{
			std::visit([&]<class T>(const T& a_item) {
				if constexpr (std::same_as<T, std::monostate>) {
					a_out = nullptr;
				} else if constexpr (std::same_as<T, std::string>) {
					a_out = RE::BSFixedString(a_item.c_str());
				} else if constexpr (std::same_as<T, FormValue>) {
					RE::BSScript::PackVariable(a_out, RE::TESForm::LookupByID(a_item.id));
				} else {
					a_out = a_item;
				}
			}, a_value);
		}

		void PackValueArray(RE::BSScript::Variable& a_out, const std::vector<Value>& a_values)
		{
			// A Papyrus Var owns its pointed-to Variable. Allocate one inner value per element; the VM array takes ownership while PackVariable builds Var[].
			std::vector<const RE::BSScript::Variable*> values;
			values.reserve(a_values.size());
			for (const auto& value : a_values) {
				auto* packed = new RE::BSScript::Variable;
				PackValue(*packed, value);
				values.push_back(packed);
			}
			RE::BSScript::PackVariable(a_out, values);
		}

		auto MakeSendArgs(RE::BSFixedString a_name, std::vector<Value> a_args, RE::BSFixedString a_sourceViewId)
		{
			return [name = std::move(a_name), args = std::move(a_args), sourceViewId = std::move(a_sourceViewId)] (RE::BSScrapArray<RE::BSScript::Variable>& a_out) -> bool {
				a_out.resize(3);
				a_out[0] = name;
				PackValueArray(a_out[1], args);
				a_out[2] = sourceViewId;
				return true;
			};
		}

		auto MakeRequestArgs(RE::BSFixedString a_name, std::vector<Value> a_args, RE::BSFixedString a_sourceViewId, RE::BSFixedString a_replyToken)
		{
			return [name = std::move(a_name), args = std::move(a_args), sourceViewId = std::move(a_sourceViewId), replyToken = std::move(a_replyToken)](RE::BSScrapArray<RE::BSScript::Variable>& a_out) -> bool {
				a_out.resize(4);
				a_out[0] = name;
				PackValueArray(a_out[1], args);
				a_out[2] = sourceViewId;
				a_out[3] = replyToken;
				return true;
			};
		}
		struct Target
		{
			Receiver          receiver;
			RE::BSFixedString fn;
		};

		// Snapshot under the lock and dispatch outside it to permit re-entry.
		std::vector<Target> CollectTargets(Kind a_kind, std::string_view a_modId, std::string_view a_key)
		{
			std::vector<Target> targets;
			std::lock_guard     l{ State().lock };
			for (const auto& e : State().entries) {
				if (e.kind != a_kind) {
					continue;
				}
				// BSFixedString casing is process-global, so match filters case-insensitively.
				if (!e.modId.empty() && !Ids::EqualsCaseInsensitiveAscii(e.modId, a_modId)) {
					continue;
				}
				if (!e.key.empty() && !Ids::EqualsCaseInsensitiveAscii(e.key, a_key)) {
					continue;
				}
				targets.emplace_back(e.receiver, e.fn);
			}
			return targets;
		}

		template <class Args>
		bool DispatchOne(VM* a_vm, const Target& a_target, Args&& a_args)
		{
			const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> noCallback{};
			const RE::BSFixedString script(a_target.receiver.script.c_str());
			if (a_target.receiver.handle == 0) {
				return a_vm->DispatchStaticCall(script, a_target.fn, std::forward<Args>(a_args), noCallback, 0);
			}
			return a_vm->DispatchMethodCall(a_target.receiver.handle, script, a_target.fn, std::forward<Args>(a_args), noCallback, 0);
		}

		auto MakeStaticCallArgs(std::vector<StaticCallArg> a_args)
		{
			return [args = std::move(a_args)](RE::BSScrapArray<RE::BSScript::Variable>& a_out) -> bool {
				using Size = RE::BSScrapArray<RE::BSScript::Variable>::size_type;
				const auto count = static_cast<Size>(args.size());
				a_out.resize(count);
				for (Size i = 0; i < count; ++i) {
					std::visit([&](const auto& value) { RE::BSScript::PackVariable(a_out[i], value); },
						args[static_cast<std::size_t>(i)]);
				}
				return true;
			};
		}

		bool Suspended() noexcept
		{
			return State().suspended.load(std::memory_order_acquire);
		}

		bool DispatchSend(std::string_view a_modId, std::string_view a_name, const std::vector<Value>& a_args, std::string_view a_sourceViewId)
		{
			if (Suspended()) {
				return false;
			}
			const auto targets = CollectTargets(Kind::kSend, a_modId, a_name);
			if (targets.empty()) {
				return false;
			}
			auto* vm = VM::GetSingleton();
			if (!vm) {
				REX::WARN("PapyrusApi: send dispatch with no VM");
				return false;
			}

			return DispatchOne(vm, targets.front(), MakeSendArgs(RE::BSFixedString(std::string(a_name).c_str()), a_args, RE::BSFixedString(std::string(a_sourceViewId).c_str())));
		}

		StaticDispatchResult DispatchViewRequestTo(const Target& a_target, std::string_view a_name, const std::vector<Value>& a_args, std::string_view a_sourceViewId, std::string_view a_deferToken)
		{
			if (Suspended()) {
				return StaticDispatchResult::kVmUnavailable;
			}
			auto* vm = VM::GetSingleton();
			if (!vm) {
				REX::WARN("PapyrusApi: view request dispatch with no VM");
				return StaticDispatchResult::kVmUnavailable;
			}

			std::string token;
			{
				std::lock_guard l{ State().lock };
				constexpr std::size_t kMaxInflightViewRequests = 256;
				constexpr std::size_t kMaxInflightPerView = 32;
				const auto perView = std::ranges::count_if(State().viewRequests, [&](const auto& item) {
					return item.second.view == a_sourceViewId;
				});
				if (State().viewRequests.size() >= kMaxInflightViewRequests || perView >= kMaxInflightPerView) {
					REX::WARN("PapyrusApi: too many view requests in flight for '{}'", a_sourceViewId);
					return StaticDispatchResult::kCapacityReached;
				}
				token = "p" + std::to_string(State().nextViewRequest++);
				PendingViewRequest pending;
				pending.token = token;
				pending.view = a_sourceViewId;
				pending.deferToken = a_deferToken;
				State().viewRequests.emplace(token, std::move(pending));
				MarkPending();
			}

			if (!DispatchOne(vm, a_target, MakeRequestArgs(RE::BSFixedString(std::string(a_name).c_str()), a_args, RE::BSFixedString(std::string(a_sourceViewId).c_str()), RE::BSFixedString(token.c_str())))) {
				std::lock_guard l{ State().lock };
				State().viewRequests.erase(token);
				return StaticDispatchResult::kTargetRejected;
			}
			return StaticDispatchResult::kQueued;
		}

		StaticDispatchResult DispatchViewRequest(std::string_view a_modId, std::string_view a_name, const std::vector<Value>& a_args, std::string_view a_sourceViewId, std::string_view a_deferToken)
		{
			const auto targets = CollectTargets(Kind::kRequest, a_modId, a_name);
			return targets.empty() ? StaticDispatchResult::kTargetRejected :
				DispatchViewRequestTo(targets.front(), a_name, a_args, a_sourceViewId, a_deferToken);
		}

		std::optional<Value> ReadPapyrusValue(const RE::BSScript::Variable* a_value, std::string_view a_native)
		{
			if (!a_value || a_value->is<std::nullptr_t>()) {
				return Value{ std::monostate{} };
			}
			if (a_value->is<bool>()) {
				return Value{ RE::BSScript::get<bool>(*a_value) };
			}
			if (a_value->is<std::int32_t>()) {
				return Value{ RE::BSScript::get<std::int32_t>(*a_value) };
			}
			if (a_value->is<float>()) {
				return Value{ RE::BSScript::get<float>(*a_value) };
			}
			if (a_value->is<RE::BSFixedString>()) {
				return Value{ std::string(RE::BSScript::get<RE::BSFixedString>(*a_value).c_str()) };
			}
			if (a_value->is<RE::BSScript::Variable>()) {
				return ReadPapyrusValue(RE::BSScript::get<RE::BSScript::Variable>(*a_value), a_native);
			}
			if (a_value->is<RE::BSScript::Object>()) {
				const auto object = RE::BSScript::get<RE::BSScript::Object>(*a_value);
				auto* type = object ? object->type.get() : nullptr;
				while (type && !Ids::EqualsCaseInsensitiveAscii(type->name.c_str(), "Form")) {
					type = type->parentTypeInfo.get();
				}
				if (!type) {
					REX::WARN("PapyrusApi: [content] {} refused an object that is not a Form", a_native);
					return std::nullopt;
				}
				const auto* form = RE::BSScript::UnpackVariable<RE::TESForm>(*a_value);
				return Value{ FormValue{ form ? static_cast<std::uint32_t>(form->GetFormID()) : 0u } };
			}
			REX::WARN("PapyrusApi: [content] {} refused an unsupported Var value", a_native);
			return std::nullopt;
		}

		nlohmann::json PlainJson(const Value& a_value)
		{
			return std::visit([]<class T>(const T& a_item) -> nlohmann::json {
				if constexpr (std::same_as<T, std::monostate>) {
					return nullptr;
				} else if constexpr (std::same_as<T, FormValue>) {
					return nullptr;  // materialized separately on the main thread
				} else {
					return a_item;
				}
			}, a_value);
		}

		bool CompleteViewRequest(const RE::BSFixedString& a_token, nlohmann::json a_value, std::optional<std::uint32_t> a_formId = std::nullopt, std::optional<std::vector<std::uint32_t>> a_formIds = std::nullopt)
		{
			const auto token = ToLowerAscii(a_token.c_str());
			std::lock_guard l{ State().lock };
			const auto it = State().viewRequests.find(token);
			if (it == State().viewRequests.end() || it->second.answered) return false;
			it->second.answered = true;
			it->second.value = std::move(a_value);
			it->second.formId = a_formId;
			it->second.formIds = std::move(a_formIds);
			MarkPending();
			return true;
		}

		bool CompleteViewRequest(const RE::BSFixedString& a_token, const Value& a_value)
		{
			if (const auto* form = std::get_if<FormValue>(&a_value)) {
				return CompleteViewRequest(a_token, nullptr, form->id);
			}
			return CompleteViewRequest(a_token, PlainJson(a_value));
		}

		bool RejectPendingViewRequest(const RE::BSFixedString& a_token,
			const RE::BSFixedString& a_code, const RE::BSFixedString& a_message)
		{
			const auto token = ToLowerAscii(a_token.c_str());
			std::lock_guard l{ State().lock };
			const auto it = State().viewRequests.find(token);
			if (it == State().viewRequests.end() || it->second.answered) return false;
			it->second.answered = true;
			it->second.rejected = true;
			it->second.code = a_code.empty() ? "papyrus-error" : a_code.c_str();
			it->second.message = a_message.c_str();
			MarkPending();
			return true;
		}
		// VM thread; normalize BSFixedString casing before validating the target.
		std::optional<std::string> FoldTarget(const RE::BSFixedString& a_mod, const RE::BSFixedString& a_key, std::string_view a_native)
		{
			auto        mod = ToLowerAscii(a_mod.c_str());
			const char* key = a_key.c_str();
			if (!Ids::IsValidModId(mod) || !key || !*key || std::string_view(key).size() > 128) {
				REX::WARN("PapyrusApi: [content] {}('{}', '{}') refused (invalid mod id or key)",
					a_native, mod.substr(0, 64), key ? std::string_view(key).substr(0, 64) : "");
				return std::nullopt;
			}
			return mod;
		}

		// Queue on the VM thread and resolve any form identity on the main thread.
		bool EnqueueState(QueuedState a_state)
		{
			std::lock_guard l{ State().lock };
			// Bound this undrained queue when the runtime is disabled.
			constexpr std::size_t kMaxPendingStates = 1024;
			if (State().states.size() >= kMaxPendingStates) {
				REX::WARN("PapyrusApi: pending view-state queue full; dropping {}.{}", a_state.mod, a_state.key);
				return false;
			}
			State().states.push_back(std::move(a_state));
			MarkPending();
			return true;
		}

		bool EnqueueState(const RE::BSFixedString& a_mod, const RE::BSFixedString& a_key,
			nlohmann::json a_value, std::string_view a_native)
		{
			auto mod = FoldTarget(a_mod, a_key, a_native);
			if (!mod) return false;
			return EnqueueState(QueuedState{ std::move(*mod), a_key.c_str(), std::move(a_value), std::nullopt, std::nullopt });
		}

		// Main thread; unknown form types fall back to their numeric value.
		std::string FormTypeSignature(RE::FormType a_type)
		{
			for (const auto& entry : RE::FORM_ENUM_STRING::GetFormEnumString()) {
				if (entry.formType == a_type && entry.formString && *entry.formString) {
					return entry.formString;
				}
			}
			return std::to_string(static_cast<std::uint32_t>(a_type));
		}

		// Main thread; missing forms serialize as null to preserve array alignment.
		nlohmann::json SerializeForm(std::uint32_t a_formId)
		{
			if (a_formId == 0) {
				return nullptr;  // None input keeps its slot
			}
			const auto* form = RE::TESForm::LookupByID(a_formId);
			if (!form) {
				REX::DEBUG("PapyrusApi: form {:#010x} vanished before serialization; delivering null slot", a_formId);
				return nullptr;
			}
			nlohmann::json out{
				{ "formId", static_cast<std::uint32_t>(form->GetFormID()) },
				{ "formType", FormTypeSignature(form->GetFormType()) },
			};
			if (const auto* fullName = starfield_cast<const RE::TESFullName*>(form)) {
				if (const char* name = fullName->GetFullName(); name && *name) {
					out["name"] = name;
				}
			}
			if (const char* editorId = form->GetFormEditorID(); editorId && *editorId) {
				out["editorId"] = editorId;  // best-effort: usually absent at runtime
			}
			return out;
		}

		// Lightweight availability/version surface shared by OSFUI.psc.
		bool IsAvailable(PapVM&, std::uint32_t, std::monostate)
		{
			return true;
		}

		std::int32_t GetVersion(PapVM&, std::uint32_t, std::monostate)
		{
			// Zero remains the documented unavailable sentinel.
			return static_cast<std::int32_t>(kOsfuiReleaseVersionMajor * 10000 +
				kOsfuiReleaseVersionMinor * 100 + kOsfuiReleaseVersionPatch);
		}

		RE::BSFixedString GetVersionString(PapVM&, std::uint32_t, std::monostate)
		{
			return RE::BSFixedString(kOsfuiReleaseVersion);
		}

		std::optional<std::string> ValidateModId(const RE::BSFixedString& a_modId)
		{
			auto modId = ToLowerAscii(a_modId.c_str());
			if (!Ids::IsValidModId(modId)) {
				return std::nullopt;
			}
			return modId;
		}

		// Papyrus namespaces use ':'; each component is an identifier.
		bool IsScriptName(std::string_view a_text)
		{
			bool first = true;
			for (const char c : a_text) {
				if (c == ':' && !first) {
					first = true;
					continue;
				}
				const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
				if (!letter && (first || c < '0' || c > '9')) return false;
				first = false;
			}
			return !first;
		}

		// VM thread. Accept only a live object the VM still binds to its handle under its own script type.
		std::optional<Receiver> ResolveInstance(PapVM& a_vm, const RE::BSTSmartPointer<RE::BSScript::Object>& a_object)
		{
			if (!a_object || !a_object->IsValid() || !a_object->type) return std::nullopt;
			const auto  handle = a_object->GetHandle();
			const auto& policy = a_vm.GetObjectHandlePolicy();
			if (!handle || handle == policy.EmptyHandle() || !policy.IsHandleObjectAvailable(handle)) return std::nullopt;
			const char* typeName = a_object->type->name.c_str();
			if (!typeName || !*typeName) return std::nullopt;
			RE::BSTSmartPointer<RE::BSScript::Object> bound;
			if (!a_vm.FindBoundObject(handle, typeName, false, bound, true) || bound.get() != a_object.get()) return std::nullopt;
			return Receiver{ handle, ToLowerAscii(typeName) };
		}

		// VM thread. A GLOBAL target must name a script the VM can load, so a typo fails here, not at dispatch.
		std::optional<Receiver> ResolveGlobal(PapVM& a_vm, const RE::BSFixedString& a_script)
		{
			const char* name = a_script.c_str();
			if (!name || !IsScriptName(name)) return std::nullopt;
			RE::BSTSmartPointer<RE::BSScript::ObjectTypeInfo> type;
			if (!a_vm.GetScriptObjectType(a_script, type) || !type) return std::nullopt;
			return Receiver{ 0, ToLowerAscii(name) };
		}

		// Idempotent per receiver: re-registering an owned endpoint succeeds, and a new instance of the same script takes it over.
		std::int32_t RegisterEndpoint(Kind a_kind, std::optional<Receiver> a_receiver, const RE::BSFixedString& a_modId, const RE::BSFixedString& a_name, std::string_view a_callback, std::string_view a_native)
		{
			if (!a_receiver) {
				REX::WARN("PapyrusApi: [content] {} refused — receiver is None or not a live script object, or the script name is empty or unknown", a_native);
				return kErrInvalidTarget;
			}
			const auto modId = ValidateModId(a_modId);
			if (!modId) {
				REX::WARN("PapyrusApi: [content] {}('{}') refused — invalid mod id", a_native,
					std::string_view(a_modId.c_str() ? a_modId.c_str() : "").substr(0, 64));
				return kErrInvalidModId;
			}
			const auto name = StringUtil::ToLowerAscii(a_name.c_str() ? a_name.c_str() : "");
			if (!IsUnreservedEndpointName(name) || modId->size() + 1 + name.size() > 128) {
				REX::WARN("PapyrusApi: [content] {}('{}', '{}') refused — reserved, malformed, or too-long endpoint name",
					a_native, *modId, std::string_view(name).substr(0, 64));
				return kErrInvalidName;
			}

			const auto qualified = *modId + "." + name;
			std::lock_guard l{ State().lock };
			for (Entry& entry : State().entries) {
				if (!Ids::EqualsCaseInsensitiveAscii(entry.modId + "." + entry.key, qualified)) {
					continue;
				}
				if (entry.kind != a_kind) {
					REX::WARN("PapyrusApi: [content] {}('{}') refused — already registered as a {} endpoint (a name cannot be both send and request)",
						a_native, qualified, entry.kind == Kind::kSend ? "send" : "request");
					return kErrConflict;
				}
				if (entry.receiver == *a_receiver) {
					REX::DEBUG("PapyrusApi: {}('{}') already registered by this script", a_native, qualified);
					return kRegistered;
				}
				// A restarted quest is a new object of the same script; let it take over instead of being blocked by its dead predecessor.
				if (entry.receiver.handle && a_receiver->handle && entry.receiver.script == a_receiver->script) {
					entry.receiver = std::move(*a_receiver);
					REX::INFO("PapyrusApi: {}('{}') rebound to a new instance of script '{}'", a_native, qualified, entry.receiver.script);
					return kRegistered;
				}
				REX::WARN("PapyrusApi: [content] {}('{}') refused — endpoint already registered by another script", a_native, qualified);
				return kErrConflict;
			}
			if (!BridgeApi::Get().ClaimPapyrusEndpoint(qualified)) return kErrConflict;
			const auto result = AddEntry(a_kind, std::move(*a_receiver), a_callback, *modId, name);
			if (result != kRegistered) BridgeApi::Get().ReleasePapyrusEndpoint(qualified);
			return result;
		}

		std::int32_t RegisterSend(PapVM& a_vm, std::uint32_t, std::monostate, RE::BSTSmartPointer<RE::BSScript::Object> a_receiver, RE::BSFixedString a_modId, RE::BSFixedString a_name)
		{
			return RegisterEndpoint(Kind::kSend, ResolveInstance(a_vm, a_receiver), a_modId, a_name, "OnOSFUISend", "RegisterSend");
		}

		std::int32_t RegisterSendStatic(PapVM& a_vm, std::uint32_t, std::monostate, RE::BSFixedString a_script, RE::BSFixedString a_modId, RE::BSFixedString a_name)
		{
			return RegisterEndpoint(Kind::kSend, ResolveGlobal(a_vm, a_script), a_modId, a_name, "OnOSFUISend", "RegisterSendStatic");
		}

		std::int32_t RegisterRequest(PapVM& a_vm, std::uint32_t, std::monostate, RE::BSTSmartPointer<RE::BSScript::Object> a_receiver, RE::BSFixedString a_modId, RE::BSFixedString a_name)
		{
			return RegisterEndpoint(Kind::kRequest, ResolveInstance(a_vm, a_receiver), a_modId, a_name, "OnOSFUIRequest", "RegisterRequest");
		}

		std::int32_t RegisterRequestStatic(PapVM& a_vm, std::uint32_t, std::monostate, RE::BSFixedString a_script, RE::BSFixedString a_modId, RE::BSFixedString a_name)
		{
			return RegisterEndpoint(Kind::kRequest, ResolveGlobal(a_vm, a_script), a_modId, a_name, "OnOSFUIRequest", "RegisterRequestStatic");
		}

		std::optional<std::vector<Value>> ReadPapyrusValues(const std::optional<std::vector<const RE::BSScript::Variable*>>& a_args, std::string_view a_native)
		{
			std::vector<Value> values;
			if (!a_args) {
				return values;
			}
			values.reserve(a_args->size());
			for (const auto* arg : *a_args) {
				auto value = ReadPapyrusValue(arg, a_native);
				if (!value) {
					return std::nullopt;
				}
				values.push_back(std::move(*value));
			}
			return values;
		}

		// Deliver one-shot events to instantiated views without caching or replay.
		bool EmitEvent(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod, RE::BSFixedString a_name, std::optional<std::vector<const RE::BSScript::Variable*>> a_args)
		{
			auto mod = FoldTarget(a_mod, a_name, "EmitEvent");
			auto args = ReadPapyrusValues(a_args, "EmitEvent");
			if (!mod || !args) return false;
			std::lock_guard l{ State().lock };
			constexpr std::size_t kMaxPendingEvents = 1024;
			if (State().events.size() >= kMaxPendingEvents) {
				REX::WARN("PapyrusApi: pending view-event queue full; dropping {}.{}", *mod, a_name.c_str());
				return false;
			}
			State().events.push_back(QueuedEvent{ std::move(*mod), StringUtil::ToLowerAscii(a_name.c_str()), std::move(*args) });
			MarkPending();
			return true;
		}

		bool SetState(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod, RE::BSFixedString a_key, const RE::BSScript::Variable* a_value)
		{
			auto mod = FoldTarget(a_mod, a_key, "SetState");
			auto value = ReadPapyrusValue(a_value, "SetState");
			if (!mod || !value) return false;
			if (const auto* form = std::get_if<FormValue>(&*value)) {
				return EnqueueState(QueuedState{ std::move(*mod), a_key.c_str(), nullptr, form->id, std::nullopt });
			}
			return EnqueueState(QueuedState{ std::move(*mod), a_key.c_str(), PlainJson(*value), std::nullopt, std::nullopt });
		}

		bool SetStateBools(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod, RE::BSFixedString a_key, std::vector<bool> a_values)
		{
			auto value = nlohmann::json::array();
			for (const bool item : a_values) value.push_back(item);
			return EnqueueState(a_mod, a_key, std::move(value), "SetStateBools");
		}

		bool SetStateInts(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod, RE::BSFixedString a_key, std::vector<std::int32_t> a_values)
		{
			return EnqueueState(a_mod, a_key, std::move(a_values), "SetStateInts");
		}

		bool SetStateFloats(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod, RE::BSFixedString a_key, std::vector<float> a_values)
		{
			return EnqueueState(a_mod, a_key, std::move(a_values), "SetStateFloats");
		}

		bool SetStateStrings(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod, RE::BSFixedString a_key, std::vector<RE::BSFixedString> a_values)
		{
			auto value = nlohmann::json::array();
			for (const auto& item : a_values) value.push_back(item.c_str());
			return EnqueueState(a_mod, a_key, std::move(value), "SetStateStrings");
		}

		bool SetStateForms(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod, RE::BSFixedString a_key, std::vector<RE::TESForm*> a_forms)
		{
			auto target = FoldTarget(a_mod, a_key, "SetStateForms");
			if (!target) return false;
			std::vector<std::uint32_t> ids;
			ids.reserve(a_forms.size());
			for (const auto* form : a_forms) {
				ids.push_back(form ? static_cast<std::uint32_t>(form->GetFormID()) : 0);
			}
			return EnqueueState(QueuedState{ std::move(*target), a_key.c_str(), nullptr, std::nullopt, std::move(ids) });
		}

		bool Reply(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_token, const RE::BSScript::Variable* a_value)
		{
			auto value = ReadPapyrusValue(a_value, "Reply");
			return value && CompleteViewRequest(a_token, *value);
		}
		bool ReplyBools(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_token, std::vector<bool> a_values)
		{
			auto value = nlohmann::json::array();
			for (const bool item : a_values) value.push_back(item);
			return CompleteViewRequest(a_token, std::move(value));
		}
		bool ReplyInts(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_token, std::vector<std::int32_t> a_values)
		{
			return CompleteViewRequest(a_token, std::move(a_values));
		}
		bool ReplyFloats(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_token, std::vector<float> a_values)
		{
			return CompleteViewRequest(a_token, std::move(a_values));
		}
		bool ReplyStrings(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_token, std::vector<RE::BSFixedString> a_values)
		{
			auto value = nlohmann::json::array();
			for (const auto& item : a_values) value.push_back(item.c_str());
			return CompleteViewRequest(a_token, std::move(value));
		}
		bool ReplyForms(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_token, std::vector<RE::TESForm*> a_forms)
		{
			std::vector<std::uint32_t> ids;
			ids.reserve(a_forms.size());
			for (const auto* form : a_forms) ids.push_back(form ? static_cast<std::uint32_t>(form->GetFormID()) : 0);
			return CompleteViewRequest(a_token, nullptr, std::nullopt, std::move(ids));
		}
		bool Reject(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_token,
			RE::BSFixedString a_code, RE::BSFixedString a_message)
		{
			return RejectPendingViewRequest(a_token, a_code, a_message);
		}
		bool Open(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_viewId)
		{
			const auto id = ToLowerAscii(a_viewId.c_str());
			return BridgeApi::Get().RequestMenu(id.c_str(), true);
		}

		bool Close(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_viewId)
		{
			const auto id = ToLowerAscii(a_viewId.c_str());
			return BridgeApi::Get().RequestMenu(id.c_str(), false);
		}

		void BindNativeMethods(PapVM* a_vm)
		{
			a_vm->BindNativeMethod(kPlatformScriptName, "IsAvailable", &IsAvailable, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "GetVersion", &GetVersion, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "GetVersionString", &GetVersionString, true, false);

			a_vm->BindNativeMethod(kPlatformScriptName, "RegisterSend", &RegisterSend, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "RegisterSendStatic", &RegisterSendStatic, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "RegisterRequest", &RegisterRequest, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "RegisterRequestStatic", &RegisterRequestStatic, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "Reply", &Reply, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "ReplyBools", &ReplyBools, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "ReplyInts", &ReplyInts, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "ReplyFloats", &ReplyFloats, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "ReplyStrings", &ReplyStrings, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "ReplyForms", &ReplyForms, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "Reject", &Reject, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "SetState", &SetState, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "SetStateBools", &SetStateBools, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "SetStateInts", &SetStateInts, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "SetStateFloats", &SetStateFloats, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "SetStateStrings", &SetStateStrings, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "SetStateForms", &SetStateForms, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "EmitEvent", &EmitEvent, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "Open", &Open, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "Close", &Close, true, false);

			REX::INFO("PapyrusApi: natives bound on script '{}'", kPlatformScriptName);
		}

		bool TryBindNatives()
		{
			if (State().nativesBound.load(std::memory_order_acquire)) {
				return true;
			}
			if (auto* gameVM = RE::GameVM::GetSingleton(); gameVM && gameVM->GetVM()) {
				BindNatives(*gameVM->GetVM());
				return true;
			}
			return false;
		}

		// Entries hold no VM references, so clearing them touches nothing the old session owned.
		void ClearRegistrations(std::string_view a_reason)
		{
			std::lock_guard l{ State().lock };
			const std::size_t dropped = State().entries.size();
			for (auto& e : State().entries) {
				BridgeApi::Get().ReleasePapyrusEndpoint(e.modId + "." + e.key);
			}
			State().entries.clear();
			// Settle old requests on the main tick and clear session identities.
			for (auto& [_, request] : State().viewRequests) {
				request.answered = true;
				request.rejected = true;
				request.code = "game-load";
				request.message = "Papyrus request was canceled by game load";
			}
			State().states.clear();
			State().events.clear();
			State().sessionReset = true;
			MarkPending();
			if (dropped) {
				REX::INFO("PapyrusApi: cleared {} script registration(s) on {} (session-scoped; scripts re-register)", dropped, a_reason);
			}
		}

		void SetSuspended(bool a_suspended, std::string_view a_reason)
		{
			if (State().suspended.exchange(a_suspended, std::memory_order_acq_rel) != a_suspended) {
				REX::DEBUG("PapyrusApi: dispatch {} ({})", a_suspended ? "suspended" : "resumed", a_reason);
			}
		}

		// Session boundaries: refuse VM dispatch while a world-replacing load is in flight, then clear stale
		// registrations before the new session runs. Natives stay bound; the VM outlives the session.
		class SessionSink final :
			public RE::BSTEventSink<RE::SaveLoadEvent>,
			public RE::BSTEventSink<RE::TESLoadGameEvent>
		{
		public:
			static SessionSink* GetSingleton()
			{
				static SessionSink* const instance = new SessionSink;
				return instance;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::SaveLoadEvent& a_event, RE::BSTEventSource<RE::SaveLoadEvent>*) override
			{
				using Op = RE::SaveLoadEvent::OpType;
				using Status = RE::SaveLoadEvent::Status;
				const bool replacesWorld = a_event.opType == Op::kLoadMostRecent || a_event.opType == Op::kQuickload ||
				                           a_event.opType == Op::kLoad || a_event.opType == Op::kLoadNamedFile ||
				                           a_event.opType == Op::kExitSaveToMainMenu || a_event.opType == Op::kExitSaveToDesktop;
				if (replacesWorld) {
					if (a_event.status == Status::kBegin) {
						SetSuspended(true, "load began");
					} else if (a_event.status == Status::kFailed || a_event.status == Status::kLoadDispatchRefused) {
						SetSuspended(false, "load failed");
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESLoadGameEvent&, RE::BSTEventSource<RE::TESLoadGameEvent>*) override
			{
				ClearRegistrations("game load");
				SetSuspended(false, "game loaded");
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void BindNatives(PapVM& a_vm)
	{
		if (State().nativesBound.exchange(true, std::memory_order_acq_rel)) {
			return;
		}
		BindNativeMethods(&a_vm);
	}

	void Install()
	{
		// Fallback for a missing bind hook: by data load the VM exists, though scripts may already have run.
		if (!TryBindNatives()) {
			REX::ERROR("PapyrusApi: GameVM unavailable at install; OSFUI natives not bound");
		}
		static bool s_sinkInstalled = false;
		if (!s_sinkInstalled) {
			if (auto* src = RE::TESLoadGameEvent::GetEventSource()) {
				src->RegisterSink(SessionSink::GetSingleton());
				s_sinkInstalled = true;
			} else {
				REX::WARN("PapyrusApi: TESLoadGameEvent source null; registrations will not clear after a game load");
			}
			if (auto* src = RE::SaveLoadEvent::GetEventSource()) {
				src->RegisterSink(SessionSink::GetSingleton());
			} else {
				REX::WARN("PapyrusApi: SaveLoadEvent source null; dispatch will not pause while a load is in flight");
			}
		}
	}

	void OnMainMenuOpened()
	{
		ClearRegistrations("main menu");
		SetSuspended(false, "main menu");
	}

	StaticDispatchResult DispatchStaticFunction(std::string_view a_script,
		std::string_view a_function, const std::vector<StaticCallArg>& a_args)
	{
		if (a_script.empty() || a_function.empty()) {
			return StaticDispatchResult::kTargetRejected;
		}
		if (Suspended()) {
			return StaticDispatchResult::kVmUnavailable;
		}
		auto* vm = VM::GetSingleton();
		if (!vm) {
			return StaticDispatchResult::kVmUnavailable;
		}

		const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> noCallback{};
		return vm->DispatchStaticCall(
			RE::BSFixedString(std::string(a_script).c_str()),
			RE::BSFixedString(std::string(a_function).c_str()),
			MakeStaticCallArgs(a_args), noCallback, 0) ?
			StaticDispatchResult::kQueued : StaticDispatchResult::kTargetRejected;
	}

	void DropViewRequest(std::string_view a_deferToken)
	{
		std::lock_guard l{ State().lock };
		std::erase_if(State().viewRequests, [&](const auto& item) { return item.second.deferToken == a_deferToken; });
	}

	ViewEndpoint ResolveViewEndpoint(std::string_view a_sourceModId, std::string_view a_name)
	{
		std::lock_guard l{ State().lock };
		const auto make = [](const Entry& a_entry) {
			return ViewEndpoint{
				a_entry.kind == Kind::kSend ? ViewEndpointKind::kSend : ViewEndpointKind::kRequest,
				a_entry.modId,
				a_entry.key,
			};
		};
		// The caller's own namespace always wins for a local name.
		for (const auto& entry : State().entries) {
			if (Ids::EqualsCaseInsensitiveAscii(entry.modId, a_sourceModId) && Ids::EqualsCaseInsensitiveAscii(entry.key, a_name)) {
				return make(entry);
			}
		}
		// Do not split at a dot: mod IDs and local endpoint names may both contain dots.
		for (const auto& entry : State().entries) {
			if (Ids::EqualsCaseInsensitiveAscii(entry.modId + "." + entry.key, a_name)) {
				return make(entry);
			}
		}
		return {};
	}

	bool OnViewSend(std::string_view a_modId, std::string_view a_name,
		const std::vector<Value>& a_args, std::string_view a_sourceViewId)
	{
		return DispatchSend(a_modId, a_name, a_args, a_sourceViewId);
	}

	StaticDispatchResult OnViewRequest(std::string_view a_modId, std::string_view a_name,
		const std::vector<Value>& a_args, std::string_view a_sourceViewId, std::string_view a_deferToken)
	{
		return DispatchViewRequest(a_modId, a_name, a_args, a_sourceViewId, a_deferToken);
	}

	PendingBatch TakePendingBatch()
	{
		PendingBatch batch;
		if (!State().pending.exchange(false, std::memory_order_acq_rel)) {
			return batch;
		}

		std::vector<QueuedState> states;
		std::vector<QueuedEvent> events;
		std::vector<PendingViewRequest> completed;
		{
			// Clear the hint before locking. A racing producer may be included in this
			// batch while leaving its bit set, which only causes one harmless extra pass.
			std::lock_guard l{ State().lock };
			states.swap(State().states);
			events.swap(State().events);
			batch.sessionReset = State().sessionReset;
			State().sessionReset = false;
			for (auto it = State().viewRequests.begin(); it != State().viewRequests.end();) {
				if (!it->second.answered) {
					++it;
					continue;
				}
				completed.push_back(std::move(it->second));
				it = State().viewRequests.erase(it);
			}
		}

		batch.states.reserve(states.size());
		for (auto& queued : states) {
			ViewState out{ std::move(queued.mod), std::move(queued.key), std::move(queued.value) };
			if (queued.formId) {
				out.value = SerializeForm(*queued.formId);
			} else if (queued.formIds) {
				auto forms = nlohmann::json::array();
				for (const auto id : *queued.formIds) forms.push_back(SerializeForm(id));
				out.value = std::move(forms);
			}
			batch.states.push_back(std::move(out));
		}

		batch.events.reserve(events.size());
		for (auto& queued : events) {
			auto args = nlohmann::json::array();
			for (const auto& value : queued.args) {
				if (const auto* form = std::get_if<FormValue>(&value)) {
					args.push_back(SerializeForm(form->id));
				} else {
					args.push_back(PlainJson(value));
				}
			}
			batch.events.push_back(ViewEvent{
				std::move(queued.mod), std::move(queued.name), std::move(args) });
		}

		batch.replies.reserve(completed.size());
		for (auto& pending : completed) {
			ViewReply reply;
			reply.view = std::move(pending.view);
			reply.deferToken = std::move(pending.deferToken);
			if (pending.rejected) {
				reply.rejected = true;
				reply.code = std::move(pending.code);
				reply.message = std::move(pending.message);
			} else if (pending.formId) {
				reply.value = SerializeForm(*pending.formId);
			} else if (pending.formIds) {
				reply.value = nlohmann::json::array();
				for (const auto id : *pending.formIds) reply.value.push_back(SerializeForm(id));
			} else {
				reply.value = std::move(pending.value);
			}
			batch.replies.push_back(std::move(reply));
		}
		return batch;
	}

}
