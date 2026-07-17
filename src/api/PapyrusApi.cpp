#include "api/PapyrusApi.h"

#include "api/BridgeApi.h"  // SettingsMirror access + RequestMenu
#include "core/Version.h"
#include "runtime/SettingsStore.h"

#include "RE/B/BSScriptUtil.h"  // BindNativeMethod marshaling, GameVM, VirtualMachine
#include "RE/E/Events.h"        // TESLoadGameEvent

namespace OSFUI::API::Papyrus
{
	namespace
	{
		using PapVM = RE::BSScript::IVirtualMachine;
		using VM = RE::BSScript::Internal::VirtualMachine;

		// The script type the natives bind to — data/Scripts/OSFUI.pex, shipped
		// with the mod. Keep in lockstep with data/Scripts/Source/OSFUI.psc.
		constexpr std::string_view kScriptName = "OSFUI";

		// ---------------------------------------------------------------- registry
		// Token registry of script callbacks (settings changes + hotkey fires),
		// modeled on the proven OSF Animation SceneEventRelay: generational
		// slots, token = (generation << 16) | slot, token 0 = failure. One
		// table for both kinds so Unregister needs no kind argument.

		enum class Kind : std::uint8_t
		{
			kSettings,
			kHotkey,
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
		};

		struct QueuedOp
		{
			std::string    mod;
			std::string    key;    // empty (reset only) = whole mod
			nlohmann::json value;  // ignored for resets
			bool           reset{ false };
		};

		// Guards the slot table AND the op queue (both are tiny; natives touch
		// them for microseconds from VM threads, the main thread drains).
		std::mutex             s_lock;
		std::vector<Entry>     s_slots;
		std::uint16_t          s_nextGen = 1;
		std::vector<QueuedOp>  s_ops;

		constexpr std::int32_t MakeToken(std::uint16_t a_gen, std::uint16_t a_slot)
		{
			return (static_cast<std::int32_t>(a_gen) << 16) | a_slot;
		}

		// Shared slot allocation + token mint. a_receiver XOR a_scriptName.
		// Caller validated the inputs; assumes s_lock held.
		std::int32_t AddEntry(Kind a_kind, const RE::BSTSmartPointer<RE::BSScript::Object>& a_receiver,
			RE::BSFixedString a_scriptName, std::string_view a_fn, std::string_view a_modId, std::string_view a_key)
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

			const auto token = MakeToken(gen, slot);
			REX::DEBUG("PapyrusApi: registered token {:#010x} -> {}{}(string, string) ({} filter '{}'{}{})",
				token, e.scriptName.empty() ? "" : std::string(e.scriptName.c_str()) + ".", e.fn.c_str(),
				a_kind == Kind::kHotkey ? "hotkey" : "settings", e.modId,
				e.key.empty() ? "" : ".", e.key);
			return token;
		}

		// A two-argument (modId, key) call. Capturing the strings keeps them
		// alive until the async VM stack consumes them.
		auto MakeArgs(RE::BSFixedString a_mod, RE::BSFixedString a_key)
		{
			return [mod = std::move(a_mod), key = std::move(a_key)](RE::BSScrapArray<RE::BSScript::Variable>& a_args) -> bool {
				a_args.resize(2);
				a_args[0] = mod;
				a_args[1] = key;
				return true;
			};
		}

		// Fan (a_modId, a_key) out to every matching registration of a_kind.
		// Snapshot under the lock, dispatch outside it (a callback may
		// re-enter Register/Unregister). DispatchMethodCall/DispatchStaticCall
		// only queue onto the VM — cheap and any-thread, though today every
		// caller is the main thread.
		void Dispatch(Kind a_kind, std::string_view a_modId, std::string_view a_key)
		{
			struct Target
			{
				RE::BSTSmartPointer<RE::BSScript::Object> receiver;
				RE::BSFixedString                         scriptName;
				RE::BSFixedString                         fn;
			};
			std::vector<Target> targets;
			{
				std::lock_guard l{ s_lock };
				for (const auto& e : s_slots) {
					if (e.generation == 0 || e.kind != a_kind) {
						continue;
					}
					// Case-insensitive: registration filters arrived through
					// BSFixedString interning, which hands back whatever casing
					// was interned FIRST process-wide — the script's literal
					// spelling never survives reliably (SettingsMirror.h).
					if (!e.modId.empty() && !EqualsCaseInsensitiveAscii(e.modId, a_modId)) {
						continue;
					}
					if (a_kind == Kind::kHotkey && !e.key.empty() && !EqualsCaseInsensitiveAscii(e.key, a_key)) {
						continue;
					}
					targets.emplace_back(e.receiver, e.scriptName, e.fn);
				}
			}
			if (targets.empty()) {
				return;
			}

			auto* vm = VM::GetSingleton();
			if (!vm) {
				REX::WARN("PapyrusApi: dispatch with no VM");
				return;
			}
			const RE::BSFixedString mod{ std::string(a_modId).c_str() };
			const RE::BSFixedString key{ std::string(a_key).c_str() };
			const RE::BSTSmartPointer<RE::BSScript::IStackCallbackFunctor> noCallback{};
			for (auto& t : targets) {
				if (!t.scriptName.empty()) {
					vm->DispatchStaticCall(t.scriptName, t.fn, MakeArgs(mod, key), noCallback, 0);
				} else {
					vm->DispatchMethodCall(t.receiver, t.fn, MakeArgs(mod, key), noCallback, 0);
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
			// Far above any legitimate burst (the drain empties this per tick).
			constexpr std::size_t kMaxPendingOps = 1024;
			if (s_ops.size() >= kMaxPendingOps) {
				REX::WARN("PapyrusApi: pending settings-op queue full; dropping Set/Reset for {}.{}", mod, key ? key : "");
				return;
			}
			s_ops.push_back({ mod, key ? key : "", std::move(a_value), a_reset });
		}

		// ---------------------------------------------------------------- natives
		// All natives run on VM tasklet threads: getters read the any-thread
		// SettingsMirror (never SettingsStore), mutations queue for the main
		// thread. Signatures follow BSScriptUtil's global-function shape.

		std::int32_t GetVersion(PapVM&, std::uint32_t, std::monostate)
		{
			// Packed for cheap compares; 0 (= native unbound, call failed)
			// means OSF UI absent — the documented feature-detect.
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
			// Papyrus int is 32-bit; schema ranges are authored well inside it,
			// but clamp rather than truncate if one ever isn't.
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
			return BridgeApi::Get().RequestMenu(a_viewId.c_str(), true);
		}

		bool CloseMenu(PapVM&, std::uint32_t, std::monostate, RE::BSFixedString a_viewId)
		{
			return BridgeApi::Get().RequestMenu(a_viewId.c_str(), false);
		}

		// ------------------------------------------------------------- lifecycle

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
			a_vm->BindNativeMethod(kScriptName, "Unregister", &Unregister, true, false);

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

		// Forget every registration WITHOUT releasing receiver refs: on a game
		// load the VM tears down first, so cached Object pointers may already
		// dangle (same reasoning + trick as OSF Animation's SceneEventRelay).
		// s_nextGen stays monotonic so a pre-load token can never validate
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

		// Game-load backstop (same pattern as OSF Animation SaveSafety): the VM
		// is rebuilt across loads, so re-bind the natives and drop the now-
		// stale script registrations before the new session's scripts run.
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
			// interning (arbitrary casing — SettingsMirror.h). An unresolvable
			// op keeps its raw names so the refusal log shows what the script
			// actually sent.
			std::string mod;
			std::string key;
			if (BridgeApi::Get().Mirror().ResolveNames(op.mod, op.key, mod, key)) {
				op.mod = std::move(mod);
				op.key = std::move(key);
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
