#include "api/PapyrusApi.h"

#include "api/BridgeApi.h"  // SettingsMirror access + RequestMenu
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
		};

		// Guards the slot table and the op/push queues: natives fill them from
		// VM threads, the main thread drains.
		std::mutex               s_lock;
		std::vector<Entry>       s_slots;
		std::uint16_t            s_nextGen = 1;
		std::vector<QueuedOp>    s_ops;
		std::vector<QueuedPush>  s_pushes;

		// Fold to the id grammar's lowercase before validating/matching: the
		// string arrived through BSFixedString interning, which hands back the
		// first-seen casing process-wide, so the script's literal spelling does
		// not survive reliably.
		std::string ToLowerAscii(std::string_view a_s)
		{
			std::string out(a_s);
			for (char& c : out) {
				if (c >= 'A' && c <= 'Z') {
					c = static_cast<char>(c + 32);
				}
			}
			return out;
		}

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
			REX::DEBUG("PapyrusApi: registered token {:#010x} -> {}{}({}) ({} filter '{}'{}{})",
				token, e.scriptName.empty() ? "" : std::string(e.scriptName.c_str()) + ".", e.fn.c_str(),
				a_wantsArgs ? "string, string[]" : "string, string",
				a_kind == Kind::kHotkey ? "hotkey" :
				a_kind == Kind::kAction ? "action" :
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
			const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> noCallback{};
			for (const auto& t : a_targets) {
				if (!t.scriptName.empty()) {
					vm->DispatchStaticCall(t.scriptName, t.fn, MakeArgs(arg1, arg2), noCallback, 0);
				} else {
					vm->DispatchMethodCall(t.receiver, t.fn, MakeArgs(arg1, arg2), noCallback, 0);
				}
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

			const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> noCallback{};
			for (const auto& t : targets) {
				if (t.wantsArgs) {
					if (!t.scriptName.empty()) {
						vm->DispatchStaticCall(t.scriptName, t.fn, MakeArgsArray(action, argv), noCallback, 0);
					} else {
						vm->DispatchMethodCall(t.receiver, t.fn, MakeArgsArray(action, argv), noCallback, 0);
					}
				} else {
					if (!t.scriptName.empty()) {
						vm->DispatchStaticCall(t.scriptName, t.fn, MakeArgs(action, scalar), noCallback, 0);
					} else {
						vm->DispatchMethodCall(t.receiver, t.fn, MakeArgs(action, scalar), noCallback, 0);
					}
				}
			}
		}

		void QueueOp(RE::BSFixedString& a_mod, RE::BSFixedString& a_key, nlohmann::json a_value, bool a_reset)
		{
			const char* mod = a_mod.c_str();
			const char* key = a_key.c_str();
			if (!mod || !*mod || (!a_reset && (!key || !*key))) {
				REX::WARN("PapyrusApi: Set/Reset with empty mod id (or Set with empty key) ignored");
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
				REX::WARN("PapyrusApi: {}('{}', '{}') refused (invalid mod id or empty key)",
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
				REX::WARN("PapyrusApi: GetFormById('{}') is not a form id", a_text.substr(0, 64));
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

		std::int32_t RegisterForViewActions(PapVM&, std::uint32_t, std::monostate,
			RE::BSTSmartPointer<RE::BSScript::Object> a_receiver, RE::BSFixedString a_fn, RE::BSFixedString a_modId)
		{
			const auto modId = ToLowerAscii(a_modId.c_str() ? a_modId.c_str() : "");
			if (!a_receiver.get() || a_fn.empty() || !Ids::IsAcceptedModId(modId)) {
				REX::DEBUG("PapyrusApi: RegisterForViewActions: null receiver, empty function name, or invalid mod id");
				return 0;
			}
			std::lock_guard l{ s_lock };
			return AddEntry(Kind::kAction, a_receiver, {}, a_fn.c_str(), modId, {});
		}

		std::int32_t RegisterForViewActionsStatic(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_script, RE::BSFixedString a_fn, RE::BSFixedString a_modId)
		{
			const auto modId = ToLowerAscii(a_modId.c_str() ? a_modId.c_str() : "");
			if (a_script.empty() || a_fn.empty() || !Ids::IsAcceptedModId(modId)) {
				REX::DEBUG("PapyrusApi: RegisterForViewActionsStatic: empty script, empty function name, or invalid mod id");
				return 0;
			}
			std::lock_guard l{ s_lock };
			return AddEntry(Kind::kAction, {}, a_script, a_fn.c_str(), modId, {});
		}

		// Args-list variants: identical to RegisterForViewActions[Static] but the
		// callback is OnUIAction(string asAction, string[] asArgs). Lets a view
		// send several values per action (osfui.send 'ui.action' { args: [...] })
		// instead of packing ints into one string.
		std::int32_t RegisterForViewActionsArgs(PapVM&, std::uint32_t, std::monostate,
			RE::BSTSmartPointer<RE::BSScript::Object> a_receiver, RE::BSFixedString a_fn, RE::BSFixedString a_modId)
		{
			const auto modId = ToLowerAscii(a_modId.c_str() ? a_modId.c_str() : "");
			if (!a_receiver.get() || a_fn.empty() || !Ids::IsAcceptedModId(modId)) {
				REX::DEBUG("PapyrusApi: RegisterForViewActionsArgs: null receiver, empty function name, or invalid mod id");
				return 0;
			}
			std::lock_guard l{ s_lock };
			return AddEntry(Kind::kAction, a_receiver, {}, a_fn.c_str(), modId, {}, true);
		}

		std::int32_t RegisterForViewActionsArgsStatic(PapVM&, std::uint32_t, std::monostate,
			RE::BSFixedString a_script, RE::BSFixedString a_fn, RE::BSFixedString a_modId)
		{
			const auto modId = ToLowerAscii(a_modId.c_str() ? a_modId.c_str() : "");
			if (a_script.empty() || a_fn.empty() || !Ids::IsAcceptedModId(modId)) {
				REX::DEBUG("PapyrusApi: RegisterForViewActionsArgsStatic: empty script, empty function name, or invalid mod id");
				return 0;
			}
			std::lock_guard l{ s_lock };
			return AddEntry(Kind::kAction, {}, a_script, a_fn.c_str(), modId, {}, true);
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
			a_vm->BindNativeMethod(kScriptName, "Unregister", &Unregister, true, false);

			a_vm->BindNativeMethod(kScriptName, "PushToView", &PushToView, true, false);
			a_vm->BindNativeMethod(kScriptName, "PushFormsToView", &PushFormsToView, true, false);
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

	void DrainViewPushes(const std::function<void(const ViewPush&)>& a_deliver)
	{
		std::vector<QueuedPush> pushes;
		{
			std::lock_guard l{ s_lock };
			pushes.swap(s_pushes);
		}
		for (auto& p : pushes) {
			ViewPush out{ std::move(p.mod), std::move(p.key), std::move(p.values), std::nullopt };
			if (p.formIds) {
				// Serialize here, on the main thread: the queue held FormIDs,
				// and a form that vanished since keeps its slot as null.
				auto forms = nlohmann::json::array();
				for (const auto id : *p.formIds) {
					forms.push_back(SerializeForm(id));
				}
				out.forms = std::move(forms);
			}
			a_deliver(out);
		}
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
					REX::WARN("PapyrusApi: Reset {}.{} refused (unknown mod/key)", op.mod, op.key.empty() ? "*" : op.key);
				}
			} else if (const auto r = a_store.SetValueWithResult(op.mod, op.key, op.value); !r.ok) {
				REX::WARN("PapyrusApi: Set {}.{} refused ({})", op.mod, op.key, r.code);
			}
		}
	}
}
