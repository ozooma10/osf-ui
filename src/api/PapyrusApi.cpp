#include "api/PapyrusApi.h"

#include "api/BridgeApi.h"  // SettingsMirror access + RequestMenu
#include "core/StringUtil.h"  // ToLowerAscii
#include "core/Version.h"
#include "runtime/Ids.h"  // id grammar validation + case-insensitive matching
#include "runtime/SettingsStore.h"

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

		// Script type the natives bind to — data/Scripts/OSFUI.pex, shipped with
		// the mod. Keep in lockstep with data/Scripts/Source/OSFUI.psc.
		constexpr std::string_view kScriptName = "OSFUI";

		// Callback registry: generational slots, token = (generation << 16) |
		// slot, token 0 = failure. One table for all kinds so Unregister needs
		// no kind argument.

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
			// kAction only: false => callback is OnUIAction(string, string) (the
			// scalar-arg shape); true => OnUIAction(string, string[]) (the
			// args-list shape from RegisterForViewActionsArgs). Both shapes can
			// coexist for one mod and each is dispatched in its declared form.
			bool wantsArgs{ false };
		};

		struct QueuedOp
		{
			std::string    mod;
			std::string    key;    // empty (reset only) = whole mod
			nlohmann::json value;  // ignored for resets
			bool           reset{ false };
		};

		// One pending PushToView/PushFormsToView. Forms are captured as FormIDs
		// (stable values) on the VM thread — never TESForm* — and serialized at
		// drain time on the main thread, where form field reads are safe.
		// has_value() on formIds marks a forms push even when the array is
		// empty ("the list is now empty").
		struct QueuedPush
		{
			std::string                                mod;
			std::string                                key;
			std::vector<std::string>                   values;
			std::optional<std::vector<std::uint32_t>>  formIds;
			std::optional<nlohmann::json>               stateValue;
			bool                                        retained{ false };
		};

		struct PendingViewRequest
		{
			std::string                               token;
			std::string                               view;
			std::string                               requestId;
			std::chrono::steady_clock::time_point     deadline;
			bool                                      answered{ false };
			bool                                      rejected{ false };
			std::string                               code;
			std::string                               message;
			nlohmann::json                            value;
			std::optional<std::vector<std::uint32_t>> formIds;
		};
		// Guards the slot table and the op/push queues: natives fill them from
		// VM threads, the main thread drains.
		std::mutex               s_lock;
		std::vector<Entry>       s_slots;
		std::uint16_t            s_nextGen = 1;
		std::vector<QueuedOp>                    s_ops;
		std::vector<QueuedPush>                    s_pushes;
		std::unordered_map<std::string, ViewPush>   s_viewState;
		std::unordered_map<std::string, PendingViewRequest> s_viewRequests;
		std::uint64_t                               s_nextViewRequest = 1;

		// Fold to the id grammar's lowercase before validating/matching: the
		// string arrived through BSFixedString interning, which hands back the
		// first-seen casing process-wide, so the script's literal spelling does
		// not survive reliably.
		using StringUtil::ToLowerAscii;

		constexpr std::int32_t MakeToken(std::uint16_t a_gen, std::uint16_t a_slot)
		{
			return (static_cast<std::int32_t>(a_gen) << 16) | a_slot;
		}

		// Slot allocation + token mint. a_receiver XOR a_scriptName. Caller
		// validated the inputs; requires s_lock held.
		std::int32_t AddEntry(Kind a_kind, const RE::BSTSmartPointer<RE::BSScript::Object>& a_receiver,
			RE::BSFixedString a_scriptName, std::string_view a_fn, std::string_view a_modId, std::string_view a_key,
			bool a_wantsArgs = false)
		{
			std::uint16_t slot = 0;
			for (; slot < s_slots.size(); slot++) {
				if (s_slots[slot].generation == 0) {
					break;
				}
			}
			if (slot == s_slots.size()) {
				if (s_slots.size() >= 0xFFFF) {
					REX::ERROR("PapyrusApi: callback table full");
					return 0;
				}
				s_slots.emplace_back();
			}

			const std::uint16_t gen = s_nextGen++;
			if (s_nextGen == 0) {
				s_nextGen = 1;  // never mint generation 0 (the empty-slot marker)
			}

			Entry& e = s_slots[slot];
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

		// A two-argument (modId, key) call. Capturing the strings by value keeps
		// them alive until the async VM stack consumes them.
		auto MakeArgs(RE::BSFixedString a_mod, RE::BSFixedString a_key)
		{
			return [mod = std::move(a_mod), key = std::move(a_key)](RE::BSScrapArray<RE::BSScript::Variable>& a_args) -> bool {
				a_args.resize(2);
				a_args[0] = mod;
				a_args[1] = key;
				return true;
			};
		}

		// The args-list action shape: OnUIAction(string asAction, string[] asArgs).
		// The Papyrus string[] is built when the closure runs on the VM thread
		// (PackVariable self-serves the VM); the value vector is copied per target.
		auto MakeArgsArray(RE::BSFixedString a_action, std::vector<RE::BSFixedString> a_args)
		{
			return [action = std::move(a_action), args = std::move(a_args)](RE::BSScrapArray<RE::BSScript::Variable>& a_out) -> bool {
				a_out.resize(2);
				a_out[0] = action;
				// PackVariable's array overload needs a non-const lvalue (its
				// concept can't form a const uninitialized probe object), and the
				// captured vector is const in this non-mutable functor — copy it.
				std::vector<RE::BSFixedString> values = args;
				RE::BSScript::PackVariable(a_out[1], values);
				return true;
			};
		}

		// Correlated request callback shape:
		// OnOSFUIViewRequest(string request, string[] args, string replyToken).
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

		// Snapshot the registrations of a_kind matching (a_modId, a_key) under
		// the lock; the caller dispatches outside it, since a callback may
		// re-enter Register/Unregister.
		std::vector<Target> CollectTargets(Kind a_kind, std::string_view a_modId, std::string_view a_key)
		{
			std::vector<Target> targets;
			std::lock_guard     l{ s_lock };
			for (const auto& e : s_slots) {
				if (e.generation == 0 || e.kind != a_kind) {
					continue;
				}
				// Case-insensitive: registration filters arrived through
				// BSFixedString interning, which hands back whatever casing
				// was interned first process-wide.
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

		// Queue one already-built args functor to one target — a static call when
		// it registered by script name, a method call otherwise. The functor (from
		// MakeArgs/MakeArgsArray) is built per call so it captures that dispatch's
		// BSFixedStrings.
		template <class Args>
		void DispatchOne(VM* a_vm, const Target& a_target, Args&& a_args)
		{
			const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> noCallback{};
			if (!a_target.scriptName.empty()) {
				a_vm->DispatchStaticCall(a_target.scriptName, a_target.fn, std::forward<Args>(a_args), noCallback, 0);
			} else {
				a_vm->DispatchMethodCall(a_target.receiver, a_target.fn, std::forward<Args>(a_args), noCallback, 0);
			}
		}

		// Queue the two-string call (a_arg1, a_arg2) to every target.
		// DispatchMethodCall/DispatchStaticCall only queue onto the VM, so this
		// is any-thread, though today every caller is the main thread.
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

		// Action shape: filter on a_modId, call each target in its declared form.
		// Scalar-arg registrants get OnUIAction(action, args[0]-or-"") — identical
		// to the pre-args behaviour; args-list registrants get the whole vector as
		// a Papyrus string[]. Both shapes can be registered for one mod at once.
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
			// Legacy scalar: the first list element (empty when the list is empty),
			// so a migrated view sending args[] still drives an unmigrated script.
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

		bool DispatchViewRequest(std::string_view a_modId, std::string_view a_request,
			const std::vector<std::string>& a_args, std::string_view a_viewId, std::string_view a_requestId)
		{
			const auto targets = CollectTargets(Kind::kRequest, a_modId, {});
			if (targets.empty()) return false;
			auto* vm = VM::GetSingleton();
			if (!vm) {
				REX::WARN("PapyrusApi: view request dispatch with no VM");
				return false;
			}

			std::string token;
			{
				std::lock_guard l{ s_lock };
				constexpr std::size_t kMaxInflightViewRequests = 256;
				constexpr std::size_t kMaxInflightPerView = 32;
				const auto perView = std::ranges::count_if(s_viewRequests, [&](const auto& item) {
					return item.second.view == a_viewId;
				});
				if (s_viewRequests.size() >= kMaxInflightViewRequests || perView >= kMaxInflightPerView) {
					REX::WARN("PapyrusApi: too many view requests in flight for '{}'", a_viewId);
					return false;
				}
				token = "p" + std::to_string(s_nextViewRequest++);
				PendingViewRequest pending;
				pending.token = token;
				pending.view = a_viewId;
				pending.requestId = a_requestId;
				pending.deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
				s_viewRequests.emplace(token, std::move(pending));
			}

			std::vector<RE::BSFixedString> argv;
			argv.reserve(a_args.size());
			for (const auto& value : a_args) argv.emplace_back(value.c_str());
			DispatchOne(vm, targets.front(), MakeRequestArgs(
				RE::BSFixedString(std::string(a_request).c_str()), std::move(argv), RE::BSFixedString(token.c_str())));
			return true;
		}

		bool CompleteViewRequest(const RE::BSFixedString& a_token, nlohmann::json a_value,
			std::optional<std::vector<std::uint32_t>> a_formIds = std::nullopt)
		{
			const auto token = ToLowerAscii(a_token.c_str() ? a_token.c_str() : "");
			std::lock_guard l{ s_lock };
			const auto it = s_viewRequests.find(token);
			if (it == s_viewRequests.end() || it->second.answered) return false;
			it->second.answered = true;
			it->second.value = std::move(a_value);
			it->second.formIds = std::move(a_formIds);
			return true;
		}

		bool RejectPendingViewRequest(const RE::BSFixedString& a_token,
			const RE::BSFixedString& a_code, const RE::BSFixedString& a_message)
		{
			const auto token = ToLowerAscii(a_token.c_str() ? a_token.c_str() : "");
			std::lock_guard l{ s_lock };
			const auto it = s_viewRequests.find(token);
			if (it == s_viewRequests.end() || it->second.answered) return false;
			it->second.answered = true;
			it->second.rejected = true;
			it->second.code = (a_code.c_str() && *a_code.c_str()) ? a_code.c_str() : "papyrus-error";
			it->second.message = a_message.c_str() ? a_message.c_str() : "";
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
			std::lock_guard l{ s_lock };
			// Drop-newest cap: with the runtime disabled via config the drain
			// never runs, and a scripted Set loop must not grow memory forever.
			// Far above any legitimate burst — the drain empties this per tick.
			constexpr std::size_t kMaxPendingOps = 1024;
			if (s_ops.size() >= kMaxPendingOps) {
				REX::WARN("PapyrusApi: pending settings-op queue full; dropping Set/Reset for {}.{}", mod, key ? key : "");
				return;
			}
			s_ops.push_back({ mod, key ? key : "", std::move(a_value), a_reset });
		}

		// Shared PushToView/PushFormsToView target validation (VM tasklet
		// thread). Fold to the grammar's lowercase before validating: the
		// interned casing is arbitrary, and the folded id is what delivery
		// prefix-matches against the (lowercase-by-grammar) view ids. Returns
		// the folded mod id, or nullopt after logging the refusal.
		std::optional<std::string> FoldPushTarget(const RE::BSFixedString& a_mod, const RE::BSFixedString& a_key, std::string_view a_native)
		{
			auto        mod = ToLowerAscii(a_mod.c_str() ? a_mod.c_str() : "");
			const char* key = a_key.c_str();
			if (!Ids::IsAcceptedModId(mod) || !key || !*key) {
				REX::WARN("PapyrusApi: [content] {}('{}', '{}') refused (invalid mod id or empty key)",
					a_native, mod.substr(0, 64), key ? std::string_view(key).substr(0, 64) : "");
				return std::nullopt;
			}
			return mod;
		}

		// Queue a validated push for Runtime::Tick's DrainViewPushes — same
		// queue-on-VM-thread / drain-on-main-thread shape as QueueOp.
		void EnqueuePush(QueuedPush a_push)
		{
			std::lock_guard l{ s_lock };
			// Same drop-newest cap as the settings-op queue, for the same
			// reason: with the runtime disabled the drain never runs.
			constexpr std::size_t kMaxPendingPushes = 1024;
			if (s_pushes.size() >= kMaxPendingPushes) {
				REX::WARN("PapyrusApi: pending view-push queue full; dropping push for {}.{}", a_push.mod, a_push.key);
				return;
			}
			s_pushes.push_back(std::move(a_push));
		}

		// SetView* is retained state, not a transient push. Values are already
		// JSON-safe on the VM thread; forms take the FormID path below because
		// their identity fields may only be read on the main thread.
		void EnqueueState(const RE::BSFixedString& a_mod, const RE::BSFixedString& a_key,
			nlohmann::json a_value, std::string_view a_native)
		{
			auto mod = FoldPushTarget(a_mod, a_key, a_native);
			if (!mod) return;
			EnqueuePush({ std::move(*mod), a_key.c_str(), {}, std::nullopt, std::move(a_value), true });
		}

		std::string StateCacheKey(std::string_view a_mod, std::string_view a_key)
		{
			std::string out;
			out.reserve(a_mod.size() + a_key.size() + 1);
			out.append(a_mod);
			out.push_back('\n');
			out.append(ToLowerAscii(a_key));
			return out;
		}
		// FormType -> 4-char record signature ("KYWD", "WEAP", ...) via the
		// game's own table. Main thread (the table is relocated game data).
		// Unknown types fall back to the numeric enum value so the field is
		// always present and stable for JS to switch on.
		std::string FormTypeSignature(RE::FormType a_type)
		{
			for (const auto& entry : RE::FORM_ENUM_STRING::GetFormEnumString()) {
				if (entry.formType == a_type && entry.formString && *entry.formString) {
					return entry.formString;
				}
			}
			return std::to_string(static_cast<std::uint32_t>(a_type));
		}

		// One element of the data.push `forms` array: the identity-only shape
		// of docs/form-references-design.md, or JSON null (a None input, or a
		// form that vanished between queue and drain) so a parallel values
		// push stays index-aligned. Main thread only — reads form fields.
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

		// GetFormById/GetFormsById body (VM tasklet thread — LookupByID is the
		// same any-thread lookup every Papyrus native uses; no field reads).
		// Accepts the two spellings a view echo can arrive in: decimal (the
		// host's number->string arg coercion) and "0x..." hex (authors quoting
		// a formId for display). None on garbage or an id that resolves to
		// nothing — the latter is the documented stale-reference case
		// (runtime FormIDs are session-scoped; see form-references-design.md).
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

		// Natives. All run on VM tasklet threads: getters read the any-thread
		// SettingsMirror (never SettingsStore), mutations queue for the main
		// thread. Signatures follow BSScriptUtil's global-function shape.

		std::int32_t GetVersion(PapVM&, std::uint32_t, std::monostate)
		{
			// Packed for cheap compares. 0 (native unbound, call failed) means
			// OSF UI absent — the documented feature-detect.
			return static_cast<std::int32_t>(kPluginVersionMajor * 10000 + kPluginVersionMinor * 100 + kPluginVersionPatch);
		}

		RE::BSFixedString GetVersionString(PapVM&, std::uint32_t, std::monostate)
		{
			return RE::BSFixedString(kPluginVersion);
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
			// Papyrus int is 32-bit; clamp rather than truncate if a schema
			// range ever exceeds it.
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
			QueueOp(a_mod, a_key, a_value.c_str() ? a_value.c_str() : "", false);
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
			std::lock_guard l{ s_lock };
			return AddEntry(Kind::kSettings, a_receiver, {}, a_fn.c_str(), a_modId.c_str() ? a_modId.c_str() : "", {});
		}

		std::int32_t RegisterForSettingChangesStatic(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_script, RE::BSFixedString a_fn, RE::BSFixedString a_modId)
		{
			if (a_script.empty() || a_fn.empty()) {
				REX::DEBUG("PapyrusApi: RegisterForSettingChangesStatic: empty script or function name");
				return 0;
			}
			std::lock_guard l{ s_lock };
			return AddEntry(Kind::kSettings, {}, a_script, a_fn.c_str(), a_modId.c_str() ? a_modId.c_str() : "", {});
		}

		std::int32_t RegisterForHotkey(PapVM&, std::uint32_t, std::monostate,
			RE::BSTSmartPointer<RE::BSScript::Object> a_receiver, RE::BSFixedString a_fn, RE::BSFixedString a_modId, RE::BSFixedString a_key)
		{
			if (!a_receiver.get() || a_fn.empty() || a_modId.empty()) {
				REX::DEBUG("PapyrusApi: RegisterForHotkey: null receiver, empty function name, or empty mod id");
				return 0;
			}
			std::lock_guard l{ s_lock };
			return AddEntry(Kind::kHotkey, a_receiver, {}, a_fn.c_str(), a_modId.c_str(), a_key.c_str() ? a_key.c_str() : "");
		}

		std::int32_t RegisterForHotkeyStatic(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_script, RE::BSFixedString a_fn, RE::BSFixedString a_modId, RE::BSFixedString a_key)
		{
			if (a_script.empty() || a_fn.empty() || a_modId.empty()) {
				REX::DEBUG("PapyrusApi: RegisterForHotkeyStatic: empty script, function name, or mod id");
				return 0;
			}
			std::lock_guard l{ s_lock };
			return AddEntry(Kind::kHotkey, {}, a_script, a_fn.c_str(), a_modId.c_str(), a_key.c_str() ? a_key.c_str() : "");
		}

		// Fold a script-supplied action mod id to the id grammar's lowercase and
		// accept only a valid one (nullopt = reject). Action registration is the
		// only Kind that validates the id up front: kSettings takes it raw as a
		// filter, kHotkey only requires it non-empty.
		std::optional<std::string> ValidateActionModId(const RE::BSFixedString& a_modId)
		{
			auto modId = ToLowerAscii(a_modId.c_str() ? a_modId.c_str() : "");
			if (!Ids::IsAcceptedModId(modId)) {
				return std::nullopt;
			}
			return modId;
		}

		// The two action-registration bodies behind the four natives below.
		// a_wantsArgs picks the callback shape; a_native names the caller so a
		// rejection still logs the exact native the script called.
		std::int32_t RegisterActionInstance(const RE::BSTSmartPointer<RE::BSScript::Object>& a_receiver,
			const RE::BSFixedString& a_fn, const RE::BSFixedString& a_modId, bool a_wantsArgs, const char* a_native)
		{
			const auto modId = ValidateActionModId(a_modId);
			if (!a_receiver.get() || a_fn.empty() || !modId) {
				REX::DEBUG("PapyrusApi: {}: null receiver, empty function name, or invalid mod id", a_native);
				return 0;
			}
			std::lock_guard l{ s_lock };
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
			std::lock_guard l{ s_lock };
			return AddEntry(Kind::kAction, {}, a_script, a_fn.c_str(), *modId, {}, a_wantsArgs);
		}

		// The four action natives. The "Args" pair differs only in the callback
		// shape: OnUIAction(string asAction, string[] asArgs) instead of a single
		// scalar, so a view can send several values per action
		// (osfui.send 'ui.action' { args: [...] }) instead of packing them into
		// one string.
		std::int32_t RegisterForViewActions(PapVM&, std::uint32_t, std::monostate,
			RE::BSTSmartPointer<RE::BSScript::Object> a_receiver, RE::BSFixedString a_fn, RE::BSFixedString a_modId)
		{
			return RegisterActionInstance(a_receiver, a_fn, a_modId, false, "RegisterForViewActions");
		}

		std::int32_t RegisterForViewActionsStatic(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_script, RE::BSFixedString a_fn, RE::BSFixedString a_modId)
		{
			return RegisterActionStatic(a_script, a_fn, a_modId, false, "RegisterForViewActionsStatic");
		}

		std::int32_t RegisterForViewActionsArgs(PapVM&, std::uint32_t, std::monostate,
			RE::BSTSmartPointer<RE::BSScript::Object> a_receiver, RE::BSFixedString a_fn, RE::BSFixedString a_modId)
		{
			return RegisterActionInstance(a_receiver, a_fn, a_modId, true, "RegisterForViewActionsArgs");
		}

		std::int32_t RegisterForViewActionsArgsStatic(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_script, RE::BSFixedString a_fn, RE::BSFixedString a_modId)
		{
			return RegisterActionStatic(a_script, a_fn, a_modId, true, "RegisterForViewActionsArgsStatic");
		}

		// Common-case registration: fixed callback name and the modern args-list
		// shape, so a script author chooses only receiver + owning mod id.
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
			std::lock_guard l{ s_lock };
			for (const auto& entry : s_slots) {
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
			std::lock_guard l{ s_lock };
			for (const auto& entry : s_slots) {
				if (entry.generation && entry.kind == Kind::kRequest &&
					Ids::EqualsCaseInsensitiveAscii(entry.modId, *modId)) {
					REX::WARN("PapyrusApi: [content] ListenForViewRequestsStatic('{}') refused — first listener wins", *modId);
					return 0;
				}
			}
			return AddEntry(Kind::kRequest, {}, a_script, "OnOSFUIViewRequest", *modId, {});
		}

		void PushToView(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_mod, RE::BSFixedString a_key, std::vector<RE::BSFixedString> a_values)
		{
			auto mod = FoldPushTarget(a_mod, a_key, "PushToView");
			if (!mod) {
				return;
			}
			std::vector<std::string> values;
			values.reserve(a_values.size());
			for (const auto& v : a_values) {
				values.emplace_back(v.c_str() ? v.c_str() : "");
			}
			EnqueuePush({ std::move(*mod), a_key.c_str(), std::move(values), std::nullopt });
		}

		// Protocol 1.3: serialize real forms into the mod's live views as the
		// data.push `forms` field. Only FormIDs are captured here on the VM
		// thread (a None element captures 0, keeping its slot); the identity
		// fields are read at drain time on the main thread.
		void PushFormsToView(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_mod, RE::BSFixedString a_key, std::vector<RE::TESForm*> a_forms)
		{
			auto mod = FoldPushTarget(a_mod, a_key, "PushFormsToView");
			if (!mod) {
				return;
			}
			std::vector<std::uint32_t> ids;
			ids.reserve(a_forms.size());
			for (const auto* form : a_forms) {
				ids.push_back(form ? static_cast<std::uint32_t>(form->GetFormID()) : 0);
			}
			EnqueuePush({ std::move(*mod), a_key.c_str(), {}, std::move(ids) });
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
			EnqueueState(a_mod, a_key, a_value.c_str() ? a_value.c_str() : "", "SetViewString");
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
			for (const auto& item : a_values) value.push_back(item.c_str() ? item.c_str() : "");
			EnqueueState(a_mod, a_key, std::move(value), "SetViewStrings");
		}

		void SetViewForms(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_mod, RE::BSFixedString a_key, std::vector<RE::TESForm*> a_forms)
		{
			auto target = FoldPushTarget(a_mod, a_key, "SetViewForms");
			if (!target) return;
			std::vector<std::uint32_t> ids;
			ids.reserve(a_forms.size());
			for (const auto* form : a_forms) {
				ids.push_back(form ? static_cast<std::uint32_t>(form->GetFormID()) : 0);
			}
			EnqueuePush({ std::move(*target), a_key.c_str(), {}, std::move(ids), std::nullopt, true });
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
			return CompleteViewRequest(a_token, a_value.c_str() ? a_value.c_str() : "");
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
			for (const auto& item : a_values) value.push_back(item.c_str() ? item.c_str() : "");
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
			return ResolveFormId(a_formId.c_str() ? a_formId.c_str() : "");
		}

		std::vector<RE::TESForm*> GetFormsById(PapVM&, std::uint32_t, std::monostate, std::vector<RE::BSFixedString> a_formIds)
		{
			std::vector<RE::TESForm*> out;
			out.reserve(a_formIds.size());
			for (const auto& id : a_formIds) {
				out.push_back(ResolveFormId(id.c_str() ? id.c_str() : ""));
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

			std::lock_guard l{ s_lock };
			if (slot >= s_slots.size() || s_slots[slot].generation != gen) {
				return false;  // stale/invalid token
			}
			s_slots[slot] = Entry{};  // generation 0 -> empty; drops the receiver smart pointer
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
			a_vm->BindNativeMethod(kScriptName, "RegisterForViewActions", &RegisterForViewActions, true, false);
			a_vm->BindNativeMethod(kScriptName, "RegisterForViewActionsStatic", &RegisterForViewActionsStatic, true, false);
			a_vm->BindNativeMethod(kScriptName, "RegisterForViewActionsArgs", &RegisterForViewActionsArgs, true, false);
			a_vm->BindNativeMethod(kScriptName, "RegisterForViewActionsArgsStatic", &RegisterForViewActionsArgsStatic, true, false);
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

			a_vm->BindNativeMethod(kScriptName, "PushToView", &PushToView, true, false);
			a_vm->BindNativeMethod(kScriptName, "PushFormsToView", &PushFormsToView, true, false);
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

		// Forget every registration without releasing receiver refs: on a game
		// load the VM tears down first, so cached Object pointers may already
		// dangle. s_nextGen stays monotonic so a pre-load token cannot validate
		// against a slot reused after the load.
		void ClearRegistrations()
		{
			std::lock_guard l{ s_lock };
			std::size_t dropped = 0;
			for (auto& e : s_slots) {
				dropped += e.generation != 0;
				std::construct_at(std::addressof(e.receiver));  // overwrite ptr = null, skip Release
			}
			s_slots.clear();
			// Retained view state and queued pushes contain session-scoped form
			// identities and must never cross a game load.
			s_viewState.clear();
			s_viewRequests.clear();
			s_pushes.clear();
			if (dropped) {
				REX::INFO("PapyrusApi: cleared {} script registration(s) on game load (session-scoped; scripts re-register)", dropped);
			}
		}

		// Game-load backstop: the VM is rebuilt across loads, so re-bind the
		// natives and drop the stale script registrations before the new
		// session's scripts run.
		class LoadGameSink final : public RE::BSTEventSink<RE::TESLoadGameEvent>
		{
		public:
			static LoadGameSink* GetSingleton()
			{
				static LoadGameSink instance;
				return &instance;
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

	void OnViewAction(std::string_view a_modId, std::string_view a_action, const std::vector<std::string>& a_args)
	{
		DispatchAction(a_modId, a_action, a_args);
	}

	bool OnViewRequest(std::string_view a_modId, std::string_view a_request,
		const std::vector<std::string>& a_args, std::string_view a_viewId, std::string_view a_requestId)
	{
		return DispatchViewRequest(a_modId, a_request, a_args, a_viewId, a_requestId);
	}

	void DrainViewReplies(const std::function<void(const ViewReply&)>& a_deliver,
		std::chrono::steady_clock::time_point a_now)
	{
		std::vector<PendingViewRequest> ready;
		{
			std::lock_guard l{ s_lock };
			for (auto it = s_viewRequests.begin(); it != s_viewRequests.end();) {
				if (!it->second.answered && a_now < it->second.deadline) {
					++it;
					continue;
				}
				ready.push_back(std::move(it->second));
				it = s_viewRequests.erase(it);
			}
		}
		for (auto& pending : ready) {
			ViewReply reply;
			reply.view = std::move(pending.view);
			reply.requestId = std::move(pending.requestId);
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

	void DrainViewPushes(const std::function<void(const ViewPush&)>& a_deliver)
	{
		std::vector<QueuedPush> pushes;
		{
			std::lock_guard l{ s_lock };
			pushes.swap(s_pushes);
		}
		for (auto& p : pushes) {
			ViewPush out{ std::move(p.mod), std::move(p.key), std::move(p.values), std::nullopt, std::move(p.stateValue) };
			if (p.formIds) {
				// Serialize here, on the main thread: the queue held FormIDs,
				// and a form that vanished since keeps its slot as null.
				auto forms = nlohmann::json::array();
				for (const auto id : *p.formIds) {
					forms.push_back(SerializeForm(id));
				}
				out.forms = forms;
				if (p.retained) out.stateValue = std::move(forms);
			}
			if (p.retained) {
				std::lock_guard l{ s_lock };
				const auto key = StateCacheKey(out.mod, out.key);
				constexpr std::size_t kMaxViewStateEntries = 1024;
				if (s_viewState.contains(key) || s_viewState.size() < kMaxViewStateEntries) {
					s_viewState[key] = out;
				} else {
					REX::WARN("PapyrusApi: retained view-state cache full; delivering but not retaining {}.{}", out.mod, out.key);
				}
			}
			a_deliver(out);
		}
	}

	void ReplayViewState(std::string_view a_modId, const std::function<void(const ViewPush&)>& a_deliver)
	{
		std::vector<ViewPush> snapshot;
		{
			std::lock_guard l{ s_lock };
			for (const auto& [_, state] : s_viewState) {
				if (Ids::EqualsCaseInsensitiveAscii(state.mod, a_modId)) snapshot.push_back(state);
			}
		}
		for (const auto& state : snapshot) a_deliver(state);
	}

	void DrainSettingsOps(SettingsStore& a_store)
	{
		std::vector<QueuedOp> ops;
		{
			std::lock_guard l{ s_lock };
			ops.swap(s_ops);
		}
		for (auto& op : ops) {
			// Canonicalize to the authored spelling before hitting the
			// case-exact store: the names arrived through BSFixedString
			// interning with arbitrary casing. An unresolvable op keeps its raw
			// names so the refusal log shows what the script actually sent.
			std::string mod;
			std::string key;
			if (BridgeApi::Get().Mirror().ResolveNames(op.mod, op.key, mod, key)) {
				op.mod = std::move(mod);
				op.key = std::move(key);
			}
			// Enum values need the same tolerance as names: "fast" can arrive
			// as "Fast" (seen in-game 2026-07-17) and enum validation is exact.
			// Canonicalize to the authored option; a real mismatch passes
			// through unchanged and refuses normally.
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
