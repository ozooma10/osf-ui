#include "API/PapyrusApi.h"

#include "API/BridgeApi.h"  // SettingsMirror access + RequestMenu
#include "Core/StringUtil.h"  // ToLowerAscii
#include "Core/Version.h"
#include "Core/Ids.h"  // opaque id safety validation + case-insensitive matching
#include "Settings/SettingsStore.h"

#include "RE/B/BSScriptUtil.h"       // BindNativeMethod marshaling, GameVM, VirtualMachine
#include "RE/E/Events.h"             // TESLoadGameEvent
#include "RE/F/FORM_ENUM_STRING.h"   // FormType -> record-signature table
#include "RE/RTTI.h"                 // starfield_cast (TESForm -> TESFullName)
#include "RE/T/TESForm.h"            // LookupByID + form identity reads
#include "RE/T/TESFullName.h"        // display-name component
#include "Compat/V1/Papyrus.h"

namespace OSFUI::API::Papyrus
{
	namespace
	{
		using PapVM = RE::BSScript::IVirtualMachine;
		using VM = RE::BSScript::Internal::VirtualMachine;

		// Keep this binding name in lockstep with data/Scripts/Source/OSFUI.psc.
		constexpr std::string_view kScriptName = kPlatformScriptName;

		// Tokens pack a generation and slot; zero is failure.

		enum class Kind : std::uint8_t
		{
			kSettings,
			kHotkey,
			kAction,  // view-fired ui.action relay: modId required, key unused
			kRequest, // correlated view request: fixed (string, string[], string) callback
		};

		struct Entry
		{
			std::uint16_t                             generation{ 0 };  // 0 = empty slot
			Kind                                      kind{ Kind::kSettings };
			RE::BSTSmartPointer<RE::BSScript::Object> receiver;    // instance target (DispatchMethodCall)
			RE::BSFixedString                         scriptName;  // set => global target (DispatchStaticCall)
			RE::BSFixedString                         fn;
			std::string                               modId;  // settings: empty = every mod; hotkey: required
			std::string                               key;    // hotkey only: empty = every key-typed setting of modId
			bool wantsArgs{ false };
		};

		struct QueuedOp
		{
			std::string    mod;
			std::string    key;    // empty (reset only) = whole mod
			nlohmann::json value;  // ignored for resets
			bool           reset{ false };
		};

		// Queue FormIDs, never TESForm pointers; serialize them on the main thread.
		struct QueuedState
		{
			std::string                               mod;
			std::string                               key;
			nlohmann::json                            value;
			std::optional<std::vector<std::uint32_t>> formIds;
		};

		// Events carry no session-scoped forms; publish form identity as retained state.
		struct QueuedEvent
		{
			std::string              mod;
			std::string              name;
			std::vector<std::string> args;
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
			std::optional<std::vector<std::uint32_t>> formIds;
		};
		// Leak this VM-owned state intentionally because process-detach destruction is unsafe.
		struct ProcessState
		{
			std::mutex                                          lock;
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

		// BSFixedString preserves process-first casing, so normalize before matching.
		using StringUtil::ToLowerAscii;

		constexpr std::int32_t MakeToken(std::uint16_t a_gen, std::uint16_t a_slot)
		{
			return (static_cast<std::int32_t>(a_gen) << 16) | a_slot;
		}

		// Caller holds the process-state lock and supplies exactly one target kind.
		std::int32_t AddEntry(Kind a_kind, const RE::BSTSmartPointer<RE::BSScript::Object>& a_receiver,
			RE::BSFixedString a_scriptName, std::string_view a_fn, std::string_view a_modId, std::string_view a_key,
			bool a_wantsArgs = false)
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
			e.wantsArgs = a_wantsArgs;

			const auto token = MakeToken(gen, slot);
			const char* signature = a_kind == Kind::kRequest ? "string, string[], string" :
				(a_wantsArgs ? "string, string[]" : "string, string");
			REX::DEBUG("PapyrusApi: registered token {:#010x} -> {}{}({}) ({} filter '{}'{}{})",
				token, e.scriptName.empty() ? "" : std::string(e.scriptName.c_str()) + ".", e.fn.c_str(),
				signature,
				a_kind == Kind::kHotkey ? "hotkey" :
				a_kind == Kind::kAction ? "action" :
				a_kind == Kind::kRequest ? "request" :
				                           "settings",
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

		// Build the Papyrus string array when the closure runs on the VM thread.
		auto MakeArgsArray(RE::BSFixedString a_action, std::vector<RE::BSFixedString> a_args)
		{
			return [action = std::move(a_action), args = std::move(a_args)](RE::BSScrapArray<RE::BSScript::Variable>& a_out) -> bool {
				a_out.resize(2);
				a_out[0] = action;
				// PackVariable requires a non-const lvalue, so copy the const capture.
				std::vector<RE::BSFixedString> values = args;
				RE::BSScript::PackVariable(a_out[1], values);
				return true;
			};
		}

		// Callback shape: OnOSFUIViewRequest(string, string[], string).
		auto MakeRequestArgs(RE::BSFixedString a_request, std::vector<RE::BSFixedString> a_args,
			RE::BSFixedString a_replyToken)
		{
			return [request = std::move(a_request), args = std::move(a_args), replyToken = std::move(a_replyToken)]
				(RE::BSScrapArray<RE::BSScript::Variable>& a_out) -> bool {
				a_out.resize(3);
				a_out[0] = request;
				std::vector<RE::BSFixedString> values = args;
				RE::BSScript::PackVariable(a_out[1], values);
				a_out[2] = replyToken;
				return true;
			};
		}
		struct Target
		{
			RE::BSTSmartPointer<RE::BSScript::Object> receiver;
			RE::BSFixedString                         scriptName;
			RE::BSFixedString                         fn;
			bool                                      wantsArgs{ false };  // kAction: string[] shape
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
				if (a_kind == Kind::kHotkey && !e.key.empty() && !Ids::EqualsCaseInsensitiveAscii(e.key, a_key)) {
					continue;
				}
				targets.emplace_back(e.receiver, e.scriptName, e.fn, e.wantsArgs);
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

		// Preserve both legacy scalar and current string-array action callbacks.
		void DispatchAction(std::string_view a_modId, std::string_view a_action, const std::vector<std::string>& a_args)
		{
			const auto targets = CollectTargets(Kind::kAction, a_modId, {});
			if (targets.empty()) {
				return;
			}
			auto* vm = VM::GetSingleton();
			if (!vm) {
				REX::WARN("PapyrusApi: action dispatch with no VM");
				return;
			}

			const RE::BSFixedString action{ std::string(a_action).c_str() };
			// Legacy scalar callbacks receive the first array element or an empty string.
			const RE::BSFixedString scalar{ a_args.empty() ? "" : a_args.front().c_str() };
			std::vector<RE::BSFixedString> argv;
			argv.reserve(a_args.size());
			for (const auto& s : a_args) {
				argv.emplace_back(s.c_str());
			}

			for (const auto& t : targets) {
				if (t.wantsArgs) {
					DispatchOne(vm, t, MakeArgsArray(action, argv));
				} else {
					DispatchOne(vm, t, MakeArgs(action, scalar));
				}
			}
		}

		StaticDispatchResult DispatchViewRequestTo(const Target& a_target, std::string_view a_request,
			const std::vector<std::string>& a_args, std::string_view a_viewId, std::string_view a_deferToken)
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
					return item.second.view == a_viewId;
				});
				if (State().viewRequests.size() >= kMaxInflightViewRequests || perView >= kMaxInflightPerView) {
					REX::WARN("PapyrusApi: too many view requests in flight for '{}'", a_viewId);
					return StaticDispatchResult::kCapacityReached;
				}
				token = "p" + std::to_string(State().nextViewRequest++);
				PendingViewRequest pending;
				pending.token = token;
				pending.view = a_viewId;
				pending.deferToken = a_deferToken;
				pending.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
				State().viewRequests.emplace(token, std::move(pending));
			}

			std::vector<RE::BSFixedString> argv;
			argv.reserve(a_args.size());
			for (const auto& value : a_args) argv.emplace_back(value.c_str());
			if (!DispatchOne(vm, a_target, MakeRequestArgs(
					RE::BSFixedString(std::string(a_request).c_str()), std::move(argv), RE::BSFixedString(token.c_str())))) {
				std::lock_guard l{ State().lock };
				State().viewRequests.erase(token);
				return StaticDispatchResult::kTargetRejected;
			}
			return StaticDispatchResult::kQueued;
		}

		bool DispatchViewRequest(std::string_view a_modId, std::string_view a_request,
			const std::vector<std::string>& a_args, std::string_view a_viewId, std::string_view a_deferToken)
		{
			const auto targets = CollectTargets(Kind::kRequest, a_modId, {});
			return !targets.empty() && DispatchViewRequestTo(
				targets.front(), a_request, a_args, a_viewId, a_deferToken) == StaticDispatchResult::kQueued;
		}

		bool CompleteViewRequest(const RE::BSFixedString& a_token, nlohmann::json a_value,
			std::optional<std::vector<std::uint32_t>> a_formIds = std::nullopt)
		{
			const auto token = ToLowerAscii(a_token.c_str());
			std::lock_guard l{ State().lock };
			const auto it = State().viewRequests.find(token);
			if (it == State().viewRequests.end() || it->second.answered) return false;
			it->second.answered = true;
			it->second.value = std::move(a_value);
			it->second.formIds = std::move(a_formIds);
			return true;
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
			return true;
		}
		void QueueOp(RE::BSFixedString& a_mod, RE::BSFixedString& a_key, nlohmann::json a_value, bool a_reset)
		{
			const char* mod = a_mod.c_str();
			const char* key = a_key.c_str();
			if (!mod || !*mod || (!a_reset && (!key || !*key))) {
				REX::WARN("PapyrusApi: [content] Set/Reset with empty mod id (or Set with empty key) ignored");
				return;
			}
			std::lock_guard l{ State().lock };
			// Bound this undrained queue when the runtime is disabled.
			constexpr std::size_t kMaxPendingOps = 1024;
			if (State().ops.size() >= kMaxPendingOps) {
				REX::WARN("PapyrusApi: pending settings-op queue full; dropping Set/Reset for {}.{}", mod, key ? key : "");
				return;
			}
			State().ops.push_back({ mod, key ? key : "", std::move(a_value), a_reset });
		}

		// VM thread; normalize BSFixedString casing before validating the target.
		std::optional<std::string> FoldTarget(const RE::BSFixedString& a_mod, const RE::BSFixedString& a_key, std::string_view a_native)
		{
			auto        mod = ToLowerAscii(a_mod.c_str());
			const char* key = a_key.c_str();
			if (!Ids::IsValidModId(mod) || !key || !*key) {
				REX::WARN("PapyrusApi: [content] {}('{}', '{}') refused (invalid mod id or empty key)",
					a_native, mod.substr(0, 64), key ? std::string_view(key).substr(0, 64) : "");
				return std::nullopt;
			}
			return mod;
		}

		// Queue on the VM thread and resolve any form identity on the main thread.
		void EnqueueState(QueuedState a_state)
		{
			std::lock_guard l{ State().lock };
			// Bound this undrained queue when the runtime is disabled.
			constexpr std::size_t kMaxPendingStates = 1024;
			if (State().states.size() >= kMaxPendingStates) {
				REX::WARN("PapyrusApi: pending view-state queue full; dropping {}.{}", a_state.mod, a_state.key);
				return;
			}
			State().states.push_back(std::move(a_state));
		}

		void EnqueueState(const RE::BSFixedString& a_mod, const RE::BSFixedString& a_key,
			nlohmann::json a_value, std::string_view a_native)
		{
			auto mod = FoldTarget(a_mod, a_key, a_native);
			if (!mod) return;
			EnqueueState(QueuedState{ std::move(*mod), a_key.c_str(), std::move(a_value), std::nullopt });
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

		// VM thread; accepts decimal or 0x-prefixed session-scoped FormIDs.
		RE::TESForm* ResolveFormId(std::string_view a_text)
		{
			const bool  hex = a_text.size() > 2 && a_text[0] == '0' && (a_text[1] == 'x' || a_text[1] == 'X');
			const char* first = a_text.data() + (hex ? 2 : 0);
			const char* last = a_text.data() + a_text.size();

			std::uint32_t id = 0;
			const auto [ptr, ec] = std::from_chars(first, last, id, hex ? 16 : 10);
			if (ec != std::errc{} || ptr != last || first == last) {
				REX::WARN("PapyrusApi: [content] GetFormById('{}') is not a form id", a_text.substr(0, 64));
				return nullptr;
			}
			if (auto* form = RE::TESForm::LookupByID(id)) {
				return form;
			}
			REX::DEBUG("PapyrusApi: GetFormById({:#010x}) resolved no form (stale reference?)", id);
			return nullptr;
		}

		// VM tasklets read the mirror and queue mutations for the main thread.

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

		void SetBool(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod, RE::BSFixedString a_key, bool a_value)
		{
			QueueOp(a_mod, a_key, a_value, false);
		}

		void SetInt(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod, RE::BSFixedString a_key, std::int32_t a_value)
		{
			QueueOp(a_mod, a_key, a_value, false);
		}

		void SetFloat(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod, RE::BSFixedString a_key, float a_value)
		{
			QueueOp(a_mod, a_key, a_value, false);
		}

		void SetString(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod, RE::BSFixedString a_key, RE::BSFixedString a_value)
		{
			QueueOp(a_mod, a_key, a_value.c_str(), false);
		}

		void Reset(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_mod, RE::BSFixedString a_key)
		{
			QueueOp(a_mod, a_key, nullptr, true);
		}

		std::int32_t RegisterForSettingChanges(PapVM&, std::uint32_t, std::monostate,
			RE::BSTSmartPointer<RE::BSScript::Object> a_receiver, RE::BSFixedString a_fn, RE::BSFixedString a_modId)
		{
			if (!a_receiver.get() || a_fn.empty()) {
				REX::DEBUG("PapyrusApi: RegisterForSettingChanges: null receiver or empty function name");
				return 0;
			}
			std::lock_guard l{ State().lock };
			return AddEntry(Kind::kSettings, a_receiver, {}, a_fn.c_str(), a_modId.c_str(), {});
		}

		std::int32_t RegisterForSettingChangesStatic(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_script, RE::BSFixedString a_fn, RE::BSFixedString a_modId)
		{
			if (a_script.empty() || a_fn.empty()) {
				REX::DEBUG("PapyrusApi: RegisterForSettingChangesStatic: empty script or function name");
				return 0;
			}
			std::lock_guard l{ State().lock };
			return AddEntry(Kind::kSettings, {}, a_script, a_fn.c_str(), a_modId.c_str(), {});
		}

		std::int32_t RegisterForHotkey(PapVM&, std::uint32_t, std::monostate,
			RE::BSTSmartPointer<RE::BSScript::Object> a_receiver, RE::BSFixedString a_fn, RE::BSFixedString a_modId, RE::BSFixedString a_key)
		{
			if (!a_receiver.get() || a_fn.empty() || a_modId.empty()) {
				REX::DEBUG("PapyrusApi: RegisterForHotkey: null receiver, empty function name, or empty mod id");
				return 0;
			}
			std::lock_guard l{ State().lock };
			return AddEntry(Kind::kHotkey, a_receiver, {}, a_fn.c_str(), a_modId.c_str(), a_key.c_str());
		}

		std::int32_t RegisterForHotkeyStatic(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_script, RE::BSFixedString a_fn, RE::BSFixedString a_modId, RE::BSFixedString a_key)
		{
			if (a_script.empty() || a_fn.empty() || a_modId.empty()) {
				REX::DEBUG("PapyrusApi: RegisterForHotkeyStatic: empty script, function name, or mod id");
				return 0;
			}
			std::lock_guard l{ State().lock };
			return AddEntry(Kind::kHotkey, {}, a_script, a_fn.c_str(), a_modId.c_str(), a_key.c_str());
		}

		// Normalize and validate action mod ids before registration.
		std::optional<std::string> ValidateActionModId(const RE::BSFixedString& a_modId)
		{
			auto modId = ToLowerAscii(a_modId.c_str());
			if (!Ids::IsValidModId(modId)) {
				return std::nullopt;
			}
			return modId;
		}

		std::int32_t RegisterActionInstance(const RE::BSTSmartPointer<RE::BSScript::Object>& a_receiver,
			const RE::BSFixedString& a_fn, const RE::BSFixedString& a_modId, bool a_wantsArgs, const char* a_native)
		{
			const auto modId = ValidateActionModId(a_modId);
			if (!a_receiver.get() || a_fn.empty() || !modId) {
				REX::DEBUG("PapyrusApi: {}: null receiver, empty function name, or invalid mod id", a_native);
				return 0;
			}
			std::lock_guard l{ State().lock };
			return AddEntry(Kind::kAction, a_receiver, {}, a_fn.c_str(), *modId, {}, a_wantsArgs);
		}

		std::int32_t RegisterActionStatic(const RE::BSFixedString& a_script,
			const RE::BSFixedString& a_fn, const RE::BSFixedString& a_modId, bool a_wantsArgs, const char* a_native)
		{
			const auto modId = ValidateActionModId(a_modId);
			if (a_script.empty() || a_fn.empty() || !modId) {
				REX::DEBUG("PapyrusApi: {}: empty script, empty function name, or invalid mod id", a_native);
				return 0;
			}
			std::lock_guard l{ State().lock };
			return AddEntry(Kind::kAction, {}, a_script, a_fn.c_str(), *modId, {}, a_wantsArgs);
		}

		// The frozen v1 adapter binds its legacy action shapes separately.
		std::int32_t ListenForViewActions(PapVM&, std::uint32_t, std::monostate,
			RE::BSTSmartPointer<RE::BSScript::Object> a_receiver, RE::BSFixedString a_modId)
		{
			return RegisterActionInstance(a_receiver, RE::BSFixedString("OnOSFUIViewAction"), a_modId,
				true, "ListenForViewActions");
		}

		std::int32_t ListenForViewActionsStatic(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_script, RE::BSFixedString a_modId)
		{
			return RegisterActionStatic(a_script, RE::BSFixedString("OnOSFUIViewAction"), a_modId,
				true, "ListenForViewActionsStatic");
		}

		std::int32_t ListenForViewRequests(PapVM&, std::uint32_t, std::monostate,
			RE::BSTSmartPointer<RE::BSScript::Object> a_receiver, RE::BSFixedString a_modId)
		{
			const auto modId = ValidateActionModId(a_modId);
			if (!a_receiver.get() || !modId) return 0;
			std::lock_guard l{ State().lock };
			for (const auto& entry : State().slots) {
				if (entry.generation && entry.kind == Kind::kRequest &&
					Ids::EqualsCaseInsensitiveAscii(entry.modId, *modId)) {
					REX::WARN("PapyrusApi: [content] ListenForViewRequests('{}') refused — first listener wins", *modId);
					return 0;
				}
			}
			return AddEntry(Kind::kRequest, a_receiver, {}, "OnOSFUIViewRequest", *modId, {});
		}

		std::int32_t ListenForViewRequestsStatic(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_script, RE::BSFixedString a_modId)
		{
			const auto modId = ValidateActionModId(a_modId);
			if (a_script.empty() || !modId) return 0;
			std::lock_guard l{ State().lock };
			for (const auto& entry : State().slots) {
				if (entry.generation && entry.kind == Kind::kRequest &&
					Ids::EqualsCaseInsensitiveAscii(entry.modId, *modId)) {
					REX::WARN("PapyrusApi: [content] ListenForViewRequestsStatic('{}') refused — first listener wins", *modId);
					return 0;
				}
			}
			return AddEntry(Kind::kRequest, {}, a_script, "OnOSFUIViewRequest", *modId, {});
		}

		// Deliver one-shot events to instantiated views without caching or replay.
		void SendViewEvent(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_mod, RE::BSFixedString a_name, std::vector<RE::BSFixedString> a_args)
		{
			auto mod = FoldTarget(a_mod, a_name, "SendViewEvent");
			if (!mod) {
				return;
			}
			std::vector<std::string> args;
			args.reserve(a_args.size());
			for (const auto& v : a_args) {
				args.emplace_back(v.c_str());
			}
			std::lock_guard l{ State().lock };
			constexpr std::size_t kMaxPendingEvents = 1024;
			if (State().events.size() >= kMaxPendingEvents) {
				REX::WARN("PapyrusApi: pending view-event queue full; dropping {}.{}", *mod, a_name.c_str());
				return;
			}
			State().events.push_back(QueuedEvent{ std::move(*mod), a_name.c_str(), std::move(args) });
		}

		void SetViewBool(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_mod, RE::BSFixedString a_key, bool a_value)
		{
			EnqueueState(a_mod, a_key, a_value, "SetViewBool");
		}

		void SetViewInt(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_mod, RE::BSFixedString a_key, std::int32_t a_value)
		{
			EnqueueState(a_mod, a_key, a_value, "SetViewInt");
		}

		void SetViewFloat(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_mod, RE::BSFixedString a_key, float a_value)
		{
			EnqueueState(a_mod, a_key, a_value, "SetViewFloat");
		}

		void SetViewString(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_mod, RE::BSFixedString a_key, RE::BSFixedString a_value)
		{
			EnqueueState(a_mod, a_key, a_value.c_str(), "SetViewString");
		}

		void SetViewBools(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_mod, RE::BSFixedString a_key, std::vector<bool> a_values)
		{
			auto value = nlohmann::json::array();
			for (const bool item : a_values) value.push_back(item);
			EnqueueState(a_mod, a_key, std::move(value), "SetViewBools");
		}

		void SetViewInts(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_mod, RE::BSFixedString a_key, std::vector<std::int32_t> a_values)
		{
			EnqueueState(a_mod, a_key, std::move(a_values), "SetViewInts");
		}

		void SetViewFloats(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_mod, RE::BSFixedString a_key, std::vector<float> a_values)
		{
			EnqueueState(a_mod, a_key, std::move(a_values), "SetViewFloats");
		}

		void SetViewStrings(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_mod, RE::BSFixedString a_key, std::vector<RE::BSFixedString> a_values)
		{
			auto value = nlohmann::json::array();
			for (const auto& item : a_values) value.push_back(item.c_str());
			EnqueueState(a_mod, a_key, std::move(value), "SetViewStrings");
		}

		void SetViewForms(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_mod, RE::BSFixedString a_key, std::vector<RE::TESForm*> a_forms)
		{
			auto target = FoldTarget(a_mod, a_key, "SetViewForms");
			if (!target) return;
			std::vector<std::uint32_t> ids;
			ids.reserve(a_forms.size());
			for (const auto* form : a_forms) {
				ids.push_back(form ? static_cast<std::uint32_t>(form->GetFormID()) : 0);
			}
			EnqueueState(QueuedState{ std::move(*target), a_key.c_str(), {}, std::move(ids) });
		}

		bool ReplyViewBool(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_token, bool a_value)
		{
			return CompleteViewRequest(a_token, a_value);
		}
		bool ReplyViewInt(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_token, std::int32_t a_value)
		{
			return CompleteViewRequest(a_token, a_value);
		}
		bool ReplyViewFloat(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_token, float a_value)
		{
			return CompleteViewRequest(a_token, a_value);
		}
		bool ReplyViewString(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_token, RE::BSFixedString a_value)
		{
			return CompleteViewRequest(a_token, a_value.c_str());
		}
		bool ReplyViewBools(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_token, std::vector<bool> a_values)
		{
			auto value = nlohmann::json::array();
			for (const bool item : a_values) value.push_back(item);
			return CompleteViewRequest(a_token, std::move(value));
		}
		bool ReplyViewInts(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_token, std::vector<std::int32_t> a_values)
		{
			return CompleteViewRequest(a_token, std::move(a_values));
		}
		bool ReplyViewFloats(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_token, std::vector<float> a_values)
		{
			return CompleteViewRequest(a_token, std::move(a_values));
		}
		bool ReplyViewStrings(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_token, std::vector<RE::BSFixedString> a_values)
		{
			auto value = nlohmann::json::array();
			for (const auto& item : a_values) value.push_back(item.c_str());
			return CompleteViewRequest(a_token, std::move(value));
		}
		bool ReplyViewForms(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_token, std::vector<RE::TESForm*> a_forms)
		{
			std::vector<std::uint32_t> ids;
			ids.reserve(a_forms.size());
			for (const auto* form : a_forms) ids.push_back(form ? static_cast<std::uint32_t>(form->GetFormID()) : 0);
			return CompleteViewRequest(a_token, nullptr, std::move(ids));
		}
		bool RejectViewRequest(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_token,
			RE::BSFixedString a_code, RE::BSFixedString a_message)
		{
			return RejectPendingViewRequest(a_token, a_code, a_message);
		}
		RE::TESForm* GetFormById(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_formId)
		{
			return ResolveFormId(a_formId.c_str());
		}

		std::vector<RE::TESForm*> GetFormsById(PapVM&, std::uint32_t, std::monostate, std::vector<RE::BSFixedString> a_formIds)
		{
			std::vector<RE::TESForm*> out;
			out.reserve(a_formIds.size());
			for (const auto& id : a_formIds) {
				out.push_back(ResolveFormId(id.c_str()));
			}
			return out;
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

		bool OpenMenu(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_viewId)
		{
			const auto id = ToLowerAscii(a_viewId.c_str());
			return BridgeApi::Get().RequestMenu(id.c_str(), true);
		}

		bool CloseMenu(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_viewId)
		{
			const auto id = ToLowerAscii(a_viewId.c_str());
			return BridgeApi::Get().RequestMenu(id.c_str(), false);
		}

		void BindNatives(PapVM* a_vm)
		{
			a_vm->BindNativeMethod(kScriptName, "GetVersion", &GetVersion, true, false);
			a_vm->BindNativeMethod(kScriptName, "GetVersionString", &GetVersionString, true, false);

			a_vm->BindNativeMethod(kScriptName, "GetBool", &GetBool, true, false);
			a_vm->BindNativeMethod(kScriptName, "GetInt", &GetInt, true, false);
			a_vm->BindNativeMethod(kScriptName, "GetFloat", &GetFloat, true, false);
			a_vm->BindNativeMethod(kScriptName, "GetString", &GetString, true, false);

			a_vm->BindNativeMethod(kScriptName, "SetBool", &SetBool, true, false);
			a_vm->BindNativeMethod(kScriptName, "SetInt", &SetInt, true, false);
			a_vm->BindNativeMethod(kScriptName, "SetFloat", &SetFloat, true, false);
			a_vm->BindNativeMethod(kScriptName, "SetString", &SetString, true, false);
			a_vm->BindNativeMethod(kScriptName, "Reset", &Reset, true, false);

			a_vm->BindNativeMethod(kScriptName, "RegisterForSettingChanges", &RegisterForSettingChanges, true, false);
			a_vm->BindNativeMethod(kScriptName, "RegisterForSettingChangesStatic", &RegisterForSettingChangesStatic, true, false);
			a_vm->BindNativeMethod(kScriptName, "RegisterForHotkey", &RegisterForHotkey, true, false);
			a_vm->BindNativeMethod(kScriptName, "RegisterForHotkeyStatic", &RegisterForHotkeyStatic, true, false);
			a_vm->BindNativeMethod(kScriptName, "ListenForViewActions", &ListenForViewActions, true, false);
			a_vm->BindNativeMethod(kScriptName, "ListenForViewActionsStatic", &ListenForViewActionsStatic, true, false);
			a_vm->BindNativeMethod(kScriptName, "ListenForViewRequests", &ListenForViewRequests, true, false);
			a_vm->BindNativeMethod(kScriptName, "ListenForViewRequestsStatic", &ListenForViewRequestsStatic, true, false);
			a_vm->BindNativeMethod(kScriptName, "ReplyViewBool", &ReplyViewBool, true, false);
			a_vm->BindNativeMethod(kScriptName, "ReplyViewInt", &ReplyViewInt, true, false);
			a_vm->BindNativeMethod(kScriptName, "ReplyViewFloat", &ReplyViewFloat, true, false);
			a_vm->BindNativeMethod(kScriptName, "ReplyViewString", &ReplyViewString, true, false);
			a_vm->BindNativeMethod(kScriptName, "ReplyViewBools", &ReplyViewBools, true, false);
			a_vm->BindNativeMethod(kScriptName, "ReplyViewInts", &ReplyViewInts, true, false);
			a_vm->BindNativeMethod(kScriptName, "ReplyViewFloats", &ReplyViewFloats, true, false);
			a_vm->BindNativeMethod(kScriptName, "ReplyViewStrings", &ReplyViewStrings, true, false);
			a_vm->BindNativeMethod(kScriptName, "ReplyViewForms", &ReplyViewForms, true, false);
			a_vm->BindNativeMethod(kScriptName, "RejectViewRequest", &RejectViewRequest, true, false);
			a_vm->BindNativeMethod(kScriptName, "Unregister", &Unregister, true, false);

			a_vm->BindNativeMethod(kScriptName, "SendViewEvent", &SendViewEvent, true, false);
			a_vm->BindNativeMethod(kScriptName, "SetViewBool", &SetViewBool, true, false);
			a_vm->BindNativeMethod(kScriptName, "SetViewInt", &SetViewInt, true, false);
			a_vm->BindNativeMethod(kScriptName, "SetViewFloat", &SetViewFloat, true, false);
			a_vm->BindNativeMethod(kScriptName, "SetViewString", &SetViewString, true, false);
			a_vm->BindNativeMethod(kScriptName, "SetViewBools", &SetViewBools, true, false);
			a_vm->BindNativeMethod(kScriptName, "SetViewInts", &SetViewInts, true, false);
			a_vm->BindNativeMethod(kScriptName, "SetViewFloats", &SetViewFloats, true, false);
			a_vm->BindNativeMethod(kScriptName, "SetViewStrings", &SetViewStrings, true, false);
			a_vm->BindNativeMethod(kScriptName, "SetViewForms", &SetViewForms, true, false);
			a_vm->BindNativeMethod(kScriptName, "GetFormById", &GetFormById, true, false);
			a_vm->BindNativeMethod(kScriptName, "GetFormsById", &GetFormsById, true, false);

			a_vm->BindNativeMethod(kScriptName, "OpenMenu", &OpenMenu, true, false);
			a_vm->BindNativeMethod(kScriptName, "CloseMenu", &CloseMenu, true, false);
			Compat::V1::Papyrus::BindNatives(a_vm);

			REX::INFO("PapyrusApi: natives bound on script '{}'", kScriptName);
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
					REX::ERROR("PapyrusApi: could not re-bind OSFUI natives after load (GameVM unavailable); "
							   "OSFUI.* stays unbound until the next successful load");
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	std::int32_t RegisterLegacyActionInstance(
		const RE::BSTSmartPointer<RE::BSScript::Object>& a_receiver,
		const RE::BSFixedString& a_fn, const RE::BSFixedString& a_mod,
		bool a_wantsArgs, const char* a_native)
	{
		return RegisterActionInstance(a_receiver, a_fn, a_mod, a_wantsArgs, a_native);
	}

	std::int32_t RegisterLegacyActionStatic(const RE::BSFixedString& a_script,
		const RE::BSFixedString& a_fn, const RE::BSFixedString& a_mod,
		bool a_wantsArgs, const char* a_native)
	{
		return RegisterActionStatic(a_script, a_fn, a_mod, a_wantsArgs, a_native);
	}

	nlohmann::json SerializeFormForLegacyPush(std::uint32_t a_formId)
	{
		return SerializeForm(a_formId);
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

	void OnViewAction(std::string_view a_modId, std::string_view a_action, const std::vector<std::string>& a_args)
	{
		DispatchAction(a_modId, a_action, a_args);
	}

	bool OnViewRequest(std::string_view a_modId, std::string_view a_request,
		const std::vector<std::string>& a_args, std::string_view a_viewId, std::string_view a_deferToken)
	{
		return DispatchViewRequest(a_modId, a_request, a_args, a_viewId, a_deferToken);
	}

	void DrainViewReplies(const std::function<void(const ViewReply&)>& a_deliver,
		std::chrono::steady_clock::time_point a_now)
	{
		std::vector<PendingViewRequest> completed;
		{
			std::lock_guard l{ State().lock };
			for (auto it = State().viewRequests.begin(); it != State().viewRequests.end();) {
				if (!it->second.answered && a_now < it->second.deadline) {
					++it;
					continue;
				}
				completed.push_back(std::move(it->second));
				it = State().viewRequests.erase(it);
			}
		}
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
			} else if (pending.formIds) {
				reply.value = nlohmann::json::array();
				for (const auto id : *pending.formIds) reply.value.push_back(SerializeForm(id));
			} else {
				reply.value = std::move(pending.value);
			}
			a_deliver(reply);
		}
	}

	void DrainViewState(const std::function<void(const ViewState&)>& a_deliver)
	{
		std::vector<QueuedState> states;
		{
			std::lock_guard l{ State().lock };
			states.swap(State().states);
		}
		for (auto& queued : states) {
			ViewState out{ std::move(queued.mod), std::move(queued.key), std::move(queued.value) };
			if (queued.formIds) {
				// Serialize on main and keep vanished forms as null to preserve alignment.
				auto forms = nlohmann::json::array();
				for (const auto id : *queued.formIds) {
					forms.push_back(SerializeForm(id));
				}
				out.value = std::move(forms);
			}
			a_deliver(out);
		}
	}

	void DrainViewEvents(const std::function<void(const ViewEvent&)>& a_deliver)
	{
		std::vector<QueuedEvent> events;
		{
			std::lock_guard l{ State().lock };
			events.swap(State().events);
		}
		for (auto& queued : events) {
			a_deliver(ViewEvent{ std::move(queued.mod), std::move(queued.name), std::move(queued.args) });
		}
	}

	bool TakeSessionReset()
	{
		std::lock_guard l{ State().lock };
		const bool pending = State().sessionReset;
		State().sessionReset = false;
		return pending;
	}

	void DrainSettingsOps(SettingsStore& a_store)
	{
		std::vector<QueuedOp> ops;
		{
			std::lock_guard l{ State().lock };
			ops.swap(State().ops);
		}
		for (auto& op : ops) {
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
