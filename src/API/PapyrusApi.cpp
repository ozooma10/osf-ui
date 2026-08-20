#include "API/PapyrusApi.h"

#include "API/BridgeApi.h"  // SettingsMirror access + RequestMenu
#include "Core/StringUtil.h"  // ToLowerAscii
#include "Core/Version.h"
#include "Core/Ids.h"  // opaque id safety validation + case-insensitive matching
#include "Settings/SettingsStore.h"

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

		// Tokens pack a generation and slot; zero is failure.

		enum class Kind : std::uint8_t
		{
			kSettings,
			kHotkey,
			kSend,
			kRequest,
		};

		struct Entry
		{
			std::uint16_t                             generation{ 0 };  // 0 = empty slot
			Kind                                      kind{ Kind::kSettings };
			RE::BSTSmartPointer<RE::BSScript::Object> receiver;    // instance target (DispatchMethodCall)
			RE::BSFixedString                         scriptName;  // set => global target (DispatchStaticCall)
			RE::BSFixedString                         fn;
			std::string                               modId;
			std::string                               key;  // setting/hotkey filter or exact view endpoint
		};

		using QueuedOp = PendingSettingsOp;

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
			std::chrono::steady_clock::time_point     deadline;
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
			std::vector<Entry>                                  slots;
			std::uint16_t                                       nextGen{ 1 };
			std::vector<QueuedOp>                               ops;
			std::vector<QueuedState>                            states;
			std::vector<QueuedEvent>                            events;
			std::unordered_map<std::string, PendingViewRequest> viewRequests;
			std::uint64_t                                       nextViewRequest{ 1 };
			// Raised on game load so Runtime::Tick purges session-scoped retained state.
			bool                                                sessionReset{ false };
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

		constexpr std::int32_t MakeToken(std::uint16_t a_gen, std::uint16_t a_slot)
		{
			return (static_cast<std::int32_t>(a_gen) << 16) | a_slot;
		}

		// Caller holds the process-state lock and supplies exactly one target kind.
		std::int32_t AddEntry(Kind a_kind, const RE::BSTSmartPointer<RE::BSScript::Object>& a_receiver, RE::BSFixedString a_scriptName, std::string_view a_fn, std::string_view a_modId, std::string_view a_key)
		{
			std::uint16_t slot = 0;
			for (; slot < State().slots.size(); slot++) {
				if (State().slots[slot].generation == 0) {
					break;
				}
			}
			if (slot == State().slots.size()) {
				if (State().slots.size() >= 0xFFFF) {
					REX::ERROR("PapyrusApi: callback table full");
					return 0;
				}
				State().slots.emplace_back();
			}

			const std::uint16_t gen = State().nextGen++;
			if (State().nextGen == 0) {
				State().nextGen = 1;  // never mint generation 0 (the empty-slot marker)
			}

			Entry& e = State().slots[slot];
			e.generation = gen;
			e.kind = a_kind;
			e.receiver = a_receiver;
			e.scriptName = a_scriptName;
			e.fn = RE::BSFixedString(std::string(a_fn).c_str());
			e.modId = std::string(a_modId);
			e.key = std::string(a_key);

			const auto token = MakeToken(gen, slot);
			const char* signature = a_kind == Kind::kRequest ? "string, Var[], string, string" : a_kind == Kind::kSend ? "string, Var[], string" : "string, string";
			REX::DEBUG("PapyrusApi: registered token {:#010x} -> {}{}({}) ({} filter '{}'{}{})",
				token, e.scriptName.empty() ? "" : std::string(e.scriptName.c_str()) + ".", e.fn.c_str(), signature,
				a_kind == Kind::kHotkey ? "hotkey" : a_kind == Kind::kSend ? "send" : a_kind == Kind::kRequest ? "request" : "settings",
				e.modId, e.key.empty() ? "" : ".", e.key);
			return token;
		}

		// Capture strings by value until the asynchronous VM consumes them.
		auto MakeArgs(RE::BSFixedString a_mod, RE::BSFixedString a_key)
		{
			return [mod = std::move(a_mod), key = std::move(a_key)](RE::BSScrapArray<RE::BSScript::Variable>& a_args) -> bool {
				a_args.resize(2);
				a_args[0] = mod;
				a_args[1] = key;
				return true;
			};
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
			RE::BSTSmartPointer<RE::BSScript::Object> receiver;
			RE::BSFixedString                         scriptName;
			RE::BSFixedString                         fn;
		};

		// Snapshot under the lock and dispatch outside it to permit re-entry.
		std::vector<Target> CollectTargets(Kind a_kind, std::string_view a_modId, std::string_view a_key)
		{
			std::vector<Target> targets;
			std::lock_guard     l{ State().lock };
			for (const auto& e : State().slots) {
				if (e.generation == 0 || e.kind != a_kind) {
					continue;
				}
				// BSFixedString casing is process-global, so match filters case-insensitively.
				if (!e.modId.empty() && !Ids::EqualsCaseInsensitiveAscii(e.modId, a_modId)) {
					continue;
				}
				if (!e.key.empty() && !Ids::EqualsCaseInsensitiveAscii(e.key, a_key)) {
					continue;
				}
				targets.emplace_back(e.receiver, e.scriptName, e.fn);
			}
			return targets;
		}

		template <class Args>
		bool DispatchOne(VM* a_vm, const Target& a_target, Args&& a_args)
		{
			const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> noCallback{};
			if (!a_target.scriptName.empty()) {
				return a_vm->DispatchStaticCall(a_target.scriptName, a_target.fn, std::forward<Args>(a_args), noCallback, 0);
			}
			return a_vm->DispatchMethodCall(a_target.receiver, a_target.fn, std::forward<Args>(a_args), noCallback, 0);
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

		// Any-thread because VM dispatch only queues the call.
		void DispatchToTargets(const std::vector<Target>& a_targets, std::string_view a_arg1, std::string_view a_arg2)
		{
			if (a_targets.empty()) {
				return;
			}
			auto* vm = VM::GetSingleton();
			if (!vm) {
				REX::WARN("PapyrusApi: dispatch with no VM");
				return;
			}
			const RE::BSFixedString arg1{ std::string(a_arg1).c_str() };
			const RE::BSFixedString arg2{ std::string(a_arg2).c_str() };
			for (const auto& t : a_targets) {
				DispatchOne(vm, t, MakeArgs(arg1, arg2));
			}
		}

		// Settings/hotkey shape: the filter values are also the call args.
		void Dispatch(Kind a_kind, std::string_view a_modId, std::string_view a_key)
		{
			DispatchToTargets(CollectTargets(a_kind, a_modId, a_key), a_modId, a_key);
		}

		bool DispatchSend(std::string_view a_modId, std::string_view a_name, const std::vector<Value>& a_args, std::string_view a_sourceViewId)
		{
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
				pending.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
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

		bool DispatchViewRequest(std::string_view a_modId, std::string_view a_name, const std::vector<Value>& a_args, std::string_view a_sourceViewId, std::string_view a_deferToken)
		{
			const auto targets = CollectTargets(Kind::kRequest, a_modId, a_name);
			return !targets.empty() && DispatchViewRequestTo(targets.front(), a_name, a_args, a_sourceViewId, a_deferToken) == StaticDispatchResult::kQueued;
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
		bool QueueOp(RE::BSFixedString& a_mod, RE::BSFixedString& a_key, nlohmann::json a_value, bool a_reset)
		{
			auto        mod = ToLowerAscii(a_mod.c_str());
			const char* key = a_key.c_str();
			const auto keyLength = key ? std::string_view(key).size() : 0;
			if (!Ids::IsValidModId(mod) || keyLength > 128 || (!a_reset && keyLength == 0)) {
				REX::WARN("PapyrusApi: [content] Set/Reset with invalid mod id (or Set with empty key) ignored");
				return false;
			}
			std::lock_guard l{ State().lock };
			// Bound this undrained queue when the runtime is disabled.
			constexpr std::size_t kMaxPendingOps = 1024;
			if (State().ops.size() >= kMaxPendingOps) {
				REX::WARN("PapyrusApi: pending settings-op queue full; dropping Set/Reset for {}.{}", mod, key ? key : "");
				return false;
			}
			State().ops.push_back({ std::move(mod), key ? key : "", std::move(a_value), a_reset });
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

		// VM tasklets read the mirror and queue mutations for the main thread.
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

		bool GetBool(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod, RE::BSFixedString a_key, bool a_default)
		{
			bool v{};
			return BridgeApi::Get().Mirror().GetBool(a_mod.c_str(), a_key.c_str(), &v) ? v : a_default;
		}

		std::int32_t GetInt(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod, RE::BSFixedString a_key, std::int32_t a_default)
		{
			std::int64_t v{};
			if (!BridgeApi::Get().Mirror().GetInt(a_mod.c_str(), a_key.c_str(), &v)) {
				return a_default;
			}
			// Clamp schema values to the 32-bit Papyrus integer range.
			return static_cast<std::int32_t>(std::clamp<std::int64_t>(v, INT32_MIN, INT32_MAX));
		}

		float GetFloat(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod, RE::BSFixedString a_key, float a_default)
		{
			double v{};
			return BridgeApi::Get().Mirror().GetFloat(a_mod.c_str(), a_key.c_str(), &v) ? static_cast<float>(v) : a_default;
		}

		RE::BSFixedString GetString(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod, RE::BSFixedString a_key, RE::BSFixedString a_default)
		{
			auto& mirror = BridgeApi::Get().Mirror();
			char  buf[256];
			const auto need = mirror.GetString(a_mod.c_str(), a_key.c_str(), buf, sizeof(buf));  // incl. NUL; 0 = unknown/mismatch
			if (need == 0) {
				return a_default;
			}
			if (need <= sizeof(buf)) {
				return RE::BSFixedString(buf);
			}
			std::string big(need - 1, '\0');
			(void)mirror.GetString(a_mod.c_str(), a_key.c_str(), big.data(), need);
			return RE::BSFixedString(big.c_str());
		}

		bool SetBool(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod, RE::BSFixedString a_key, bool a_value)
		{
			return QueueOp(a_mod, a_key, a_value, false);
		}

		bool SetInt(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod, RE::BSFixedString a_key, std::int32_t a_value)
		{
			return QueueOp(a_mod, a_key, a_value, false);
		}

		bool SetFloat(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod, RE::BSFixedString a_key, float a_value)
		{
			return QueueOp(a_mod, a_key, a_value, false);
		}

		bool SetString(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod, RE::BSFixedString a_key, RE::BSFixedString a_value)
		{
			return QueueOp(a_mod, a_key, a_value.c_str(), false);
		}

		bool Reset(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod, RE::BSFixedString a_key)
		{
			return QueueOp(a_mod, a_key, nullptr, true);
		}

		std::optional<std::string> ValidateModId(const RE::BSFixedString& a_modId)
		{
			auto modId = ToLowerAscii(a_modId.c_str());
			if (!Ids::IsValidModId(modId)) {
				return std::nullopt;
			}
			return modId;
		}

		std::int32_t RegisterFixedListener(Kind a_kind, const RE::BSTSmartPointer<RE::BSScript::Object>& a_receiver, const RE::BSFixedString& a_script, const RE::BSFixedString& a_modId, const RE::BSFixedString& a_key, std::string_view a_callback, std::string_view a_native)
		{
			const auto modId = ValidateModId(a_modId);
			if ((!a_receiver.get() && a_script.empty()) || !modId || std::string_view(a_key.c_str()).size() > 128) {
				REX::DEBUG("PapyrusApi: {}: missing target, invalid mod id, or key too long", a_native);
				return 0;
			}
			std::lock_guard l{ State().lock };
			return AddEntry(a_kind, a_receiver, a_script, a_callback, *modId, a_key.c_str());
		}

		std::int32_t ListenForChanges(PapVM&, std::uint32_t, std::monostate, RE::BSTSmartPointer<RE::BSScript::Object> a_receiver, RE::BSFixedString a_modId, RE::BSFixedString a_key)
		{
			return RegisterFixedListener(Kind::kSettings, a_receiver, {}, a_modId, a_key, "OnOSFUISettingChanged", "ListenForChanges");
		}

		std::int32_t ListenForChangesStatic(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_script, RE::BSFixedString a_modId, RE::BSFixedString a_key)
		{
			return RegisterFixedListener(Kind::kSettings, {}, a_script, a_modId, a_key, "OnOSFUISettingChanged", "ListenForChangesStatic");
		}

		std::int32_t ListenForHotkeys(PapVM&, std::uint32_t, std::monostate, RE::BSTSmartPointer<RE::BSScript::Object> a_receiver, RE::BSFixedString a_modId, RE::BSFixedString a_key)
		{
			return RegisterFixedListener(Kind::kHotkey, a_receiver, {}, a_modId, a_key, "OnOSFUIHotkey", "ListenForHotkeys");
		}

		std::int32_t ListenForHotkeysStatic(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_script, RE::BSFixedString a_modId, RE::BSFixedString a_key)
		{
			return RegisterFixedListener(Kind::kHotkey, {}, a_script, a_modId, a_key, "OnOSFUIHotkey", "ListenForHotkeysStatic");
		}

		std::int32_t RegisterEndpoint(Kind a_kind, const RE::BSTSmartPointer<RE::BSScript::Object>& a_receiver, const RE::BSFixedString& a_script, const RE::BSFixedString& a_modId, const RE::BSFixedString& a_name, std::string_view a_callback, std::string_view a_native)
		{
			const auto modId = ValidateModId(a_modId);
			const std::string name(a_name.c_str());
			const auto qualifiedLength = modId ? modId->size() + 1 + name.size() : 0;
			if ((!a_receiver.get() && a_script.empty()) || !modId || !IsUnreservedEndpointName(name) || qualifiedLength > 128) {
				REX::DEBUG("PapyrusApi: {}: missing target, invalid mod id, reserved endpoint, or qualified name too long", a_native);
				return 0;
			}
			const auto qualified = *modId + "." + name;
			std::lock_guard l{ State().lock };
			for (const auto& entry : State().slots) {
				if (!entry.generation || (entry.kind != Kind::kSend && entry.kind != Kind::kRequest)) {
					continue;
				}
				if (Ids::EqualsCaseInsensitiveAscii(entry.modId + "." + entry.key, qualified)) {
					REX::WARN("PapyrusApi: [content] {}('{}') refused — endpoint already registered (first wins)", a_native, qualified);
					return 0;
				}
			}
			return AddEntry(a_kind, a_receiver, a_script, a_callback, *modId, name);
		}

		std::int32_t RegisterSend(PapVM&, std::uint32_t, std::monostate, RE::BSTSmartPointer<RE::BSScript::Object> a_receiver, RE::BSFixedString a_modId, RE::BSFixedString a_name)
		{
			return RegisterEndpoint(Kind::kSend, a_receiver, {}, a_modId, a_name, "OnOSFUISend", "RegisterSend");
		}

		std::int32_t RegisterSendStatic(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_script, RE::BSFixedString a_modId, RE::BSFixedString a_name)
		{
			return RegisterEndpoint(Kind::kSend, {}, a_script, a_modId, a_name, "OnOSFUISend", "RegisterSendStatic");
		}

		std::int32_t RegisterRequest(PapVM&, std::uint32_t, std::monostate, RE::BSTSmartPointer<RE::BSScript::Object> a_receiver, RE::BSFixedString a_modId, RE::BSFixedString a_name)
		{
			return RegisterEndpoint(Kind::kRequest, a_receiver, {}, a_modId, a_name, "OnOSFUIRequest", "RegisterRequest");
		}

		std::int32_t RegisterRequestStatic(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_script, RE::BSFixedString a_modId, RE::BSFixedString a_name)
		{
			return RegisterEndpoint(Kind::kRequest, {}, a_script, a_modId, a_name, "OnOSFUIRequest", "RegisterRequestStatic");
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
			State().events.push_back(QueuedEvent{ std::move(*mod), a_name.c_str(), std::move(*args) });
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
		bool Unregister(PapVM&, std::uint32_t, std::monostate, std::int32_t a_token)
		{
			if (a_token == 0) {
				return false;
			}
			const auto slot = static_cast<std::uint16_t>(a_token & 0xFFFF);
			const auto gen = static_cast<std::uint16_t>((a_token >> 16) & 0xFFFF);

			std::lock_guard l{ State().lock };
			if (slot >= State().slots.size() || State().slots[slot].generation != gen) {
				return false;  // stale/invalid token
			}
			State().slots[slot] = Entry{};  // generation 0 -> empty; drops the receiver smart pointer
			REX::DEBUG("PapyrusApi: unregistered token {:#010x}", a_token);
			return true;
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

		void BindNatives(PapVM* a_vm)
		{
			a_vm->BindNativeMethod(kPlatformScriptName, "IsAvailable", &IsAvailable, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "GetVersion", &GetVersion, true, false);
			a_vm->BindNativeMethod(kPlatformScriptName, "GetVersionString", &GetVersionString, true, false);

			a_vm->BindNativeMethod(kSettingsScriptName, "GetBool", &GetBool, true, false);
			a_vm->BindNativeMethod(kSettingsScriptName, "GetInt", &GetInt, true, false);
			a_vm->BindNativeMethod(kSettingsScriptName, "GetFloat", &GetFloat, true, false);
			a_vm->BindNativeMethod(kSettingsScriptName, "GetString", &GetString, true, false);
			a_vm->BindNativeMethod(kSettingsScriptName, "SetBool", &SetBool, true, false);
			a_vm->BindNativeMethod(kSettingsScriptName, "SetInt", &SetInt, true, false);
			a_vm->BindNativeMethod(kSettingsScriptName, "SetFloat", &SetFloat, true, false);
			a_vm->BindNativeMethod(kSettingsScriptName, "SetString", &SetString, true, false);
			a_vm->BindNativeMethod(kSettingsScriptName, "Reset", &Reset, true, false);
			a_vm->BindNativeMethod(kSettingsScriptName, "ListenForChanges", &ListenForChanges, true, false);
			a_vm->BindNativeMethod(kSettingsScriptName, "ListenForChangesStatic", &ListenForChangesStatic, true, false);
			a_vm->BindNativeMethod(kSettingsScriptName, "ListenForHotkeys", &ListenForHotkeys, true, false);
			a_vm->BindNativeMethod(kSettingsScriptName, "ListenForHotkeysStatic", &ListenForHotkeysStatic, true, false);
			a_vm->BindNativeMethod(kSettingsScriptName, "Unregister", &Unregister, true, false);

			a_vm->BindNativeMethod(kViewScriptName, "RegisterSend", &RegisterSend, true, false);
			a_vm->BindNativeMethod(kViewScriptName, "RegisterSendStatic", &RegisterSendStatic, true, false);
			a_vm->BindNativeMethod(kViewScriptName, "RegisterRequest", &RegisterRequest, true, false);
			a_vm->BindNativeMethod(kViewScriptName, "RegisterRequestStatic", &RegisterRequestStatic, true, false);
			a_vm->BindNativeMethod(kViewScriptName, "Reply", &Reply, true, false);
			a_vm->BindNativeMethod(kViewScriptName, "ReplyBools", &ReplyBools, true, false);
			a_vm->BindNativeMethod(kViewScriptName, "ReplyInts", &ReplyInts, true, false);
			a_vm->BindNativeMethod(kViewScriptName, "ReplyFloats", &ReplyFloats, true, false);
			a_vm->BindNativeMethod(kViewScriptName, "ReplyStrings", &ReplyStrings, true, false);
			a_vm->BindNativeMethod(kViewScriptName, "ReplyForms", &ReplyForms, true, false);
			a_vm->BindNativeMethod(kViewScriptName, "Reject", &Reject, true, false);
			a_vm->BindNativeMethod(kViewScriptName, "SetState", &SetState, true, false);
			a_vm->BindNativeMethod(kViewScriptName, "SetStateBools", &SetStateBools, true, false);
			a_vm->BindNativeMethod(kViewScriptName, "SetStateInts", &SetStateInts, true, false);
			a_vm->BindNativeMethod(kViewScriptName, "SetStateFloats", &SetStateFloats, true, false);
			a_vm->BindNativeMethod(kViewScriptName, "SetStateStrings", &SetStateStrings, true, false);
			a_vm->BindNativeMethod(kViewScriptName, "SetStateForms", &SetStateForms, true, false);
			a_vm->BindNativeMethod(kViewScriptName, "EmitEvent", &EmitEvent, true, false);
			a_vm->BindNativeMethod(kViewScriptName, "Open", &Open, true, false);
			a_vm->BindNativeMethod(kViewScriptName, "Close", &Close, true, false);
			a_vm->BindNativeMethod(kViewScriptName, "Unregister", &Unregister, true, false);

			REX::INFO("PapyrusApi: natives bound on scripts '{}', '{}', and '{}'", kPlatformScriptName, kSettingsScriptName, kViewScriptName);
		}

		bool TryBindNatives()
		{
			if (auto* gameVM = RE::GameVM::GetSingleton(); gameVM && gameVM->GetVM()) {
				BindNatives(gameVM->GetVM());
				return true;
			}
			return false;
		}

		// Clear after VM teardown without releasing stale receivers or reusing old tokens.
		void ClearRegistrations()
		{
			std::lock_guard l{ State().lock };
			std::size_t dropped = 0;
			for (auto& e : State().slots) {
				dropped += e.generation != 0;
				std::construct_at(std::addressof(e.receiver));  // overwrite ptr = null, skip Release
			}
			State().slots.clear();
			// Drop queued session identities and signal the runtime to clear retained copies.
			State().viewRequests.clear();
			State().states.clear();
			State().events.clear();
			State().sessionReset = true;
			MarkPending();
			if (dropped) {
				REX::INFO("PapyrusApi: cleared {} script registration(s) on game load (session-scoped; scripts re-register)", dropped);
			}
		}

		// Rebind natives and clear stale registrations before the new session runs.
		class LoadGameSink final : public RE::BSTEventSink<RE::TESLoadGameEvent>
		{
		public:
			static LoadGameSink* GetSingleton()
			{
				static LoadGameSink* const instance = new LoadGameSink;
				return instance;
			}

			RE::BSEventNotifyControl ProcessEvent(const RE::TESLoadGameEvent&, RE::BSTEventSource<RE::TESLoadGameEvent>*) override
			{
				ClearRegistrations();
				if (!TryBindNatives()) {
					REX::ERROR("PapyrusApi: could not re-bind native scripts after load (GameVM unavailable)");
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Install()
	{
		if (!TryBindNatives()) {
			REX::ERROR("PapyrusApi: GameVM unavailable at install; OSFUI natives not bound");
		}
		static bool s_sinkInstalled = false;
		if (!s_sinkInstalled) {
			if (auto* src = RE::TESLoadGameEvent::GetEventSource()) {
				src->RegisterSink(LoadGameSink::GetSingleton());
				s_sinkInstalled = true;
			} else {
				REX::WARN("PapyrusApi: TESLoadGameEvent source null; natives will not re-bind after a game load");
			}
		}
	}

	void OnSettingChanged(std::string_view a_modId, std::string_view a_key)
	{
		Dispatch(Kind::kSettings, a_modId, a_key);
	}

	void OnHotkey(std::string_view a_modId, std::string_view a_key)
	{
		Dispatch(Kind::kHotkey, a_modId, a_key);
	}

	StaticDispatchResult DispatchStaticHotkey(std::string_view a_script,
		std::string_view a_function, std::string_view a_modId, std::string_view a_key)
	{
		return DispatchStaticFunction(a_script, a_function,
			{ std::string(a_modId), std::string(a_key) });
	}

	StaticDispatchResult DispatchStaticFunction(std::string_view a_script,
		std::string_view a_function, const std::vector<StaticCallArg>& a_args)
	{
		if (a_script.empty() || a_function.empty()) {
			return StaticDispatchResult::kTargetRejected;
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
		for (const auto& entry : State().slots) {
			if (!entry.generation || (entry.kind != Kind::kSend && entry.kind != Kind::kRequest)) continue;
			if (Ids::EqualsCaseInsensitiveAscii(entry.modId, a_sourceModId) && Ids::EqualsCaseInsensitiveAscii(entry.key, a_name)) {
				return make(entry);
			}
		}
		// Do not split at a dot: mod IDs and local endpoint names may both contain dots.
		for (const auto& entry : State().slots) {
			if (!entry.generation || (entry.kind != Kind::kSend && entry.kind != Kind::kRequest)) continue;
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

	bool OnViewRequest(std::string_view a_modId, std::string_view a_name,
		const std::vector<Value>& a_args, std::string_view a_sourceViewId, std::string_view a_deferToken)
	{
		return DispatchViewRequest(a_modId, a_name, a_args, a_sourceViewId, a_deferToken);
	}

	PendingBatch TakePendingBatch(std::chrono::steady_clock::time_point a_now)
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
			batch.settings.swap(State().ops);
			states.swap(State().states);
			events.swap(State().events);
			batch.sessionReset = State().sessionReset;
			State().sessionReset = false;
			for (auto it = State().viewRequests.begin(); it != State().viewRequests.end();) {
				if (!it->second.answered && a_now < it->second.deadline) {
					++it;
					continue;
				}
				completed.push_back(std::move(it->second));
				it = State().viewRequests.erase(it);
			}
			if (!State().viewRequests.empty()) {
				MarkPending();
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
			if (!pending.answered) {
				reply.rejected = true;
				reply.code = "papyrus-timeout";
				reply.message = "Papyrus did not answer the view request";
			} else if (pending.rejected) {
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

	void ApplySettingsOps(std::vector<PendingSettingsOp> a_ops, SettingsStore& a_store)
	{
		for (auto& op : a_ops) {
			// Restore authored casing before the case-exact store lookup.
			std::string mod;
			std::string key;
			if (BridgeApi::Get().Mirror().ResolveNames(op.mod, op.key, mod, key)) {
				op.mod = std::move(mod);
				op.key = std::move(key);
			}
			// Canonicalize enum casing altered by BSFixedString interning.
			if (!op.reset && op.value.is_string()) {
				if (auto canon = a_store.CanonicalEnumValue(op.mod, op.key, op.value.get_ref<const std::string&>())) {
					op.value = std::move(*canon);
				}
			}
			if (op.reset) {
				if (!a_store.Reset(op.mod, op.key)) {
					REX::WARN("PapyrusApi: [content] Reset {}.{} refused (unknown mod/key)", op.mod, op.key.empty() ? "*" : op.key);
				}
			} else if (const auto r = a_store.SetValueWithResult(op.mod, op.key, op.value); !r.ok) {
				REX::WARN("PapyrusApi: [content] Set {}.{} refused ({})", op.mod, op.key, r.code);
			}
		}
	}
}
