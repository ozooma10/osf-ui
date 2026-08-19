#include "Bindings/LiveControlMap.h"

#include "Core/Log.h"
#include "Input/KeyNames.h"

#include "RE/B/BSScaleformManager.h"
#include "RE/C/ControlMap.h"
#include "REL/ASM.h"
#include "REL/Relocation.h"
#include "REL/Trampoline.h"
#include "REX/FModule.h"
#include "SFSE/InputMap.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <tuple>
#include <type_traits>

namespace OSFUI
{
	namespace
	{
		constexpr auto kRemapQuietPeriod = std::chrono::milliseconds(150);
		constexpr auto kRemapMaximumDelay = std::chrono::seconds(1);
		constexpr std::size_t kRemapDispatchPrologueSize = 6;

		struct RawBinding
		{
			std::uint8_t slot{};
			std::uint32_t keyCode{};
			std::uint32_t modifierCode{};
		};

		struct GroupedRow
		{
			std::uint8_t engineInputContextId{};
			std::uint8_t sortIndex{ 0xFF };
			bool required{ false };
			std::string event;
			std::vector<RawBinding> keyboard;
		};

		using RemapDispatchFn = void (*)(const void*);
		std::atomic<std::uintptr_t> g_remapGateway{ 0 };
		std::atomic<std::uint64_t> g_remapGeneration{ 0 };

		void HashByte(std::uint64_t& a_hash, std::uint8_t a_value)
		{
			a_hash ^= a_value;
			a_hash *= 1099511628211ull;
		}

		template <class T>
		void HashInteger(std::uint64_t& a_hash, T a_value)
		{
			using U = std::make_unsigned_t<T>;
			const auto value = static_cast<U>(a_value);
			for (std::size_t i = 0; i < sizeof(U); ++i) {
				HashByte(a_hash, static_cast<std::uint8_t>(value >> (i * 8)));
			}
		}

		std::uint64_t HashRows(const std::vector<GroupedRow>& a_rows)
		{
			std::uint64_t hash = 14695981039346656037ull;
			for (const auto& row : a_rows) {
				HashInteger(hash, row.engineInputContextId);
				HashInteger(hash, row.sortIndex);
				HashInteger(hash, static_cast<std::uint8_t>(row.required));
				HashInteger(hash, static_cast<std::uint32_t>(row.event.size()));
				for (const auto ch : row.event) HashByte(hash, static_cast<std::uint8_t>(ch));
				HashInteger(hash, static_cast<std::uint32_t>(row.keyboard.size()));
				for (const auto& binding : row.keyboard) {
					HashInteger(hash, binding.slot);
					HashInteger(hash, binding.keyCode);
					HashInteger(hash, binding.modifierCode);
				}
			}
			return hash;
		}

		std::uint32_t EngineInputContextOrder(std::uint8_t a_engineInputContextId)
		{
			return RE::ControlMap::GetControlsMenuOrder(
				static_cast<RE::ControlMap::InputContextID>(a_engineInputContextId));
		}

		std::uint8_t EngineInputCategoryOwner(std::uint8_t a_engineInputContextId)
		{
			const auto context = static_cast<RE::ControlMap::InputContextID>(a_engineInputContextId);
			return static_cast<std::uint8_t>(RE::ControlMap::GetControlsMenuCategory(context));
		}

		std::string Translate(RE::BSScaleformManager* a_manager, std::string_view a_token)
		{
			if (!a_manager) return std::string(a_token);
			std::wstring wide;
			if (!REX::UTF8_TO_UTF16(a_token, wide)) return std::string(a_token);
			const auto result = a_manager->Translate(wide.c_str());
			std::string translated;
			return REX::UTF16_TO_UTF8(std::wstring_view{ result.data(), result.length() }, translated) ? translated : std::string(a_token);
		}

		nlohmann::json EncodeModes(GameplayModeMask a_modes)
		{
			nlohmann::json out = nlohmann::json::array();
			for (const auto mode : { GameplayMode::OnFoot, GameplayMode::Ship, GameplayMode::Vehicle, GameplayMode::ZeroG }) {
				if ((a_modes & ModeBit(mode)) != 0) out.push_back(GameplayModeName(mode));
			}
			return out;
		}

		void RemapDispatchHook(const void* a_event)
		{
			if (const auto gateway = g_remapGateway.load(std::memory_order_acquire)) {
				reinterpret_cast<RemapDispatchFn>(gateway)(a_event);
			}
			g_remapGeneration.fetch_add(1, std::memory_order_release);
		}
	}

	void LiveControlMap::EncodeUnavailableStates()
	{
		_keybindingsState = {
			{ "available", false }, { "revision", _revision }, { "gameVersion", _gameVersion },
			{ "error", _failureReason }, { "actions", nlohmann::json::array() },
		};
		_engineInputContextState = {
			{ "available", false }, { "revision", _engineInputContextRevision }, { "mode", nullptr },
			{ "contexts", nlohmann::json::array() },
		};
	}

	void LiveControlMap::Fail(std::string a_reason)
	{
		_available = false;
		_failureReason = std::move(a_reason);
		_conflicts.clear();
		_remapPending = false;
		_pendingRemapEdges = 0;
		_activeEngineInputContexts.clear();
		_mode.reset();
		++_revision;
		++_engineInputContextRevision;
		EncodeUnavailableStates();
		REX::ERROR("LiveControlMap: unavailable -- {} (Starfield {})", _failureReason, _gameVersion);
	}

	void LiveControlMap::Initialize()
	{
		if (_initialized) return;
		const auto game = REX::FModule::GetLoadedModule("Starfield.exe");
		const auto version = game.GetFileVersion();
		_gameVersion = version.string();
		if (RebuildBindings(/*forceProjection*/ true) == RebuildResult::Failed || !RefreshActiveEngineInputContexts()) {
			_initialized = true;
			return;
		}
		InstallRemapObserver();
		_seenRemapGeneration = g_remapGeneration.load(std::memory_order_acquire);
		_initialized = true;
		REX::INFO("LiveControlMap: game-binding catalog published -- {} visible actions, revision {}, mode {}", _keybindingsState["actions"].size(), _revision, _mode ? GameplayModeName(*_mode) : "unknown");
	}

	void LiveControlMap::InstallRemapObserver()
	{
		if (g_remapGateway.load(std::memory_order_acquire)) return;
		REL::Relocation<std::uintptr_t> target{ RE::ID::ControlsRemappedEvent::Dispatch };
		auto& trampoline = REL::GetTrampoline();
		auto* gateway = static_cast<std::byte*>(trampoline.allocate(kRemapDispatchPrologueSize + sizeof(REL::ASM::JMP14)));
		std::memcpy(gateway, reinterpret_cast<const void*>(target.address()), kRemapDispatchPrologueSize);
		const REL::ASM::JMP14 jumpBack{ target.address() + kRemapDispatchPrologueSize };
		std::memcpy(gateway + kRemapDispatchPrologueSize, std::addressof(jumpBack), sizeof(jumpBack));
		g_remapGateway.store(reinterpret_cast<std::uintptr_t>(gateway), std::memory_order_release);
		target.write_jmp<5>(RemapDispatchHook);
		REX::INFO("LiveControlMap: observing ControlsRemappedEvent dispatch");
	}

	LiveControlMap::RebuildResult LiveControlMap::RebuildBindings(bool a_forceProjection)
	{
		using Clock = std::chrono::steady_clock;
		const auto started = Clock::now();
		const auto millis = [](Clock::time_point a_begin, Clock::time_point a_end) {
			return std::chrono::duration_cast<std::chrono::milliseconds>(a_end - a_begin).count();
		};
		const auto* controlMap = RE::ControlMap::GetSingleton();
		if (!controlMap) {
			Fail("ControlMap singleton is unavailable");
			return RebuildResult::Failed;
		}

		std::array<std::string, RE::ControlMap::INPUT_CONTEXT_NAME_COUNT> engineInputContextNames;
		for (std::size_t i = 0; i < engineInputContextNames.size(); ++i) {
			const auto id = static_cast<RE::ControlMap::InputContextID>(i);
			engineInputContextNames[i] = RE::ControlMap::GetInputContextName(id);
		}

		std::map<std::pair<std::uint8_t, std::string>, GroupedRow> grouped;
		constexpr std::array devices{
			RE::InputEvent::DeviceType::kKeyboard,
			RE::InputEvent::DeviceType::kMouse,
		};
		for (std::uint8_t engineInputContextId = 0;
			 engineInputContextId < RE::ControlMap::MAPPABLE_INPUT_CONTEXT_COUNT;
			 ++engineInputContextId) {
			const auto context = static_cast<RE::ControlMap::InputContextID>(engineInputContextId);
			for (const auto device : devices) {
				for (const auto& mapping : controlMap->GetMappings(context, device)) {
					if (!mapping.visibleInControls) continue;
					const std::string event{ mapping.eventID.c_str(), mapping.eventID.length() };
					auto& row = grouped[{ engineInputContextId, event }];
					row.engineInputContextId = engineInputContextId;
					row.event = event;
					row.sortIndex = (std::min)(row.sortIndex, mapping.sortIndex);
					row.required = row.required || mapping.required;
					if (device == RE::InputEvent::DeviceType::kKeyboard &&
						mapping.bindingSlot != RE::ControlMap::BindingSlot::kUnbound) {
						row.keyboard.push_back({ static_cast<std::uint8_t>(mapping.bindingSlot), mapping.keyCode, mapping.modifierKeyCode });
					}
				}
			}
		}
		const auto enumerated = Clock::now();

		std::vector<GroupedRow> rows;
		rows.reserve(grouped.size());
		for (auto& [key, row] : grouped) {
			(void)key;
			std::ranges::sort(row.keyboard, [](const RawBinding& a, const RawBinding& b) {
				return std::tie(a.slot, a.keyCode, a.modifierCode) < std::tie(b.slot, b.keyCode, b.modifierCode);
			});
			rows.push_back(std::move(row));
		}
		std::ranges::sort(rows, [](const GroupedRow& a, const GroupedRow& b) {
			return std::tuple{ EngineInputContextOrder(a.engineInputContextId), a.sortIndex, a.event } < std::tuple{ EngineInputContextOrder(b.engineInputContextId), b.sortIndex, b.event };
		});
		const auto structuralHash = HashRows(rows);
		const auto sorted = Clock::now();
		if (!a_forceProjection && _available && structuralHash == _bindingHash) {
			_failureReason.clear();
			REX::INFO("LiveControlMap: dirty snapshot unchanged in {} ms (enumerate {}, sort/hash {}); skipped translation, JSON, and broadcasts", millis(started, sorted), millis(started, enumerated), millis(enumerated, sorted));
			return RebuildResult::Unchanged;
		}

		nlohmann::json actions = nlohmann::json::array();
		std::vector<ConflictBinding> conflicts;
		auto* scaleformManager = RE::BSScaleformManager::GetSingleton();
		std::size_t translationHits = 0;
		std::size_t translationCalls = 0;
		auto translate = [&](const std::string& a_token) -> std::string {
			if (const auto found = _translationCache.find(a_token); found != _translationCache.end()) {
				++translationHits;
				return found->second;
			}
			++translationCalls;
			auto value = Translate(scaleformManager, a_token);
			// Do not persist raw fallbacks from a transiently unavailable translator.
			if (scaleformManager) _translationCache.emplace(a_token, value);
			return value;
		};
		for (const auto& row : rows) {
			const auto& engineInputContextName = engineInputContextNames[row.engineInputContextId];
			const auto& categoryName = engineInputContextNames[EngineInputCategoryOwner(row.engineInputContextId)];
			const auto policy = ControlMapPolicy::Classify(row.engineInputContextId, engineInputContextName);
			const auto baseToken = std::format("${}_{}", engineInputContextName, row.event);
			auto label = translate(baseToken + "_KBM");
			if (label.empty() || label == baseToken + "_KBM") label = translate(baseToken);
			if (label.empty() || label == baseToken) label = row.event;
			const auto categoryToken = "$" + categoryName;
			auto category = translate(categoryToken);
			if (category.empty() || category == categoryToken) category = categoryName;

			nlohmann::json bindings = nlohmann::json::array();
			if (row.keyboard.empty()) {
				bindings.push_back({ { "slot", "main" }, { "key", nullptr }, { "chord", nlohmann::json::array() }, { "unbound", true } });
			}
			for (const auto& raw : row.keyboard) {
				const auto code = SFSE::InputMap::VirtualKeyToKeycode(raw.keyCode);
				const auto modifier = SFSE::InputMap::VirtualKeyToKeycode(raw.modifierCode);
				const auto slot = raw.slot == 0 ? "main" : "alternate";
				nlohmann::json chord = nlohmann::json::array();
				if (modifier) chord.push_back(KeyName(static_cast<ScanCode>(modifier)));
				if (code) chord.push_back(KeyName(static_cast<ScanCode>(code)));
				bindings.push_back({
					{ "slot", slot }, { "key", code ? nlohmann::json(KeyName(static_cast<ScanCode>(code))) : nlohmann::json(nullptr) },
					{ "chord", std::move(chord) }, { "unbound", code == 0 },
				});
				// ControlMap chords are display-only. Single-key main and alternate bindings both participate in conflict analysis.
				if (code && !modifier) {
					conflicts.push_back({ row.event, label, engineInputContextName, slot, code, policy.classification, policy.definiteModes, policy.possibleModes });
				}
			}

			// `context` is a frozen public state key. Its value is specifically an engine input context, despite the historical unqualified spelling.
			actions.push_back({
				{ "event", row.event }, { "label", label }, { "category", category },
				{ "context", { { "id", row.engineInputContextId }, { "name", engineInputContextName }, { "order", EngineInputContextOrder(row.engineInputContextId) } } },
				{ "classification", ControlMapPolicy::ClassificationName(policy.classification) },
				{ "modes", { { "definite", EncodeModes(policy.definiteModes) }, { "possible", EncodeModes(policy.possibleModes) } } },
				{ "sortIndex", row.sortIndex }, { "required", row.required }, { "bindings", std::move(bindings) },
			});
		}

		const auto actionCount = actions.size();
		_conflicts = std::move(conflicts);
		_bindingHash = structuralHash;
		_available = true;
		_failureReason.clear();
		++_revision;
		_keybindingsState = {
			{ "available", true }, { "revision", _revision }, { "gameVersion", _gameVersion },
			{ "actions", std::move(actions) },
		};
		const auto projected = Clock::now();
		REX::INFO("LiveControlMap: rebuilt {} visible actions in {} ms (enumerate {}, sort/hash {}, project {}; translations {} engine / {} cached)", actionCount, millis(started, projected), millis(started, enumerated), millis(enumerated, sorted), millis(sorted, projected), translationCalls, translationHits);
		return RebuildResult::Changed;
	}

	bool LiveControlMap::RefreshActiveEngineInputContexts()
	{
		if (!_available) return false;
		const auto* controlMap = RE::ControlMap::GetSingleton();
		if (!controlMap) {
			Fail("ControlMap singleton is unavailable");
			return false;
		}

		const auto activeContexts = controlMap->GetActiveInputContexts();
		std::vector<std::uint8_t> active;
		active.reserve(activeContexts.size());
		for (const auto& context : activeContexts) {
			active.push_back(static_cast<std::uint8_t>(context.inputContextID));
		}
		const std::span<const std::uint8_t> activeSpan{ active };
		const auto mode = ControlMapPolicy::DeriveMode(activeSpan);
		if (_activeEngineInputContexts == active && mode == _mode && !_engineInputContextState.empty()) return false;
		_activeEngineInputContexts = std::move(active);
		_mode = mode;
		nlohmann::json activeEngineInputContexts = nlohmann::json::array();
		for (const auto id : _activeEngineInputContexts) {
			const auto context = static_cast<RE::ControlMap::InputContextID>(id);
			activeEngineInputContexts.push_back({ { "id", id }, { "name", RE::ControlMap::GetInputContextName(context) } });
		}
		++_engineInputContextRevision;
		// `contexts` is a frozen public state key in the osfui/input-context document; every entry is an active engine input context.
		_engineInputContextState = {
			{ "available", true }, { "revision", _engineInputContextRevision },
			{ "mode", _mode ? nlohmann::json(GameplayModeName(*_mode)) : nlohmann::json(nullptr) },
			{ "contexts", std::move(activeEngineInputContexts) },
		};
		return true;
	}

	LiveControlMap::Changes LiveControlMap::Pump()
	{
		Changes changes;
		if (!_initialized || !_available) return changes;
		const auto now = std::chrono::steady_clock::now();
		const auto generation = g_remapGeneration.load(std::memory_order_acquire);
		if (generation != _seenRemapGeneration) {
			_pendingRemapEdges += generation - _seenRemapGeneration;
			_seenRemapGeneration = generation;
			if (!_remapPending) {
				_remapPending = true;
				_firstRemapEdge = now;
			}
			_lastRemapEdge = now;
		}
		if (_remapPending &&
			(now - _lastRemapEdge >= kRemapQuietPeriod || now - _firstRemapEdge >= kRemapMaximumDelay)) {
			const auto edges = _pendingRemapEdges;
			_pendingRemapEdges = 0;
			_remapPending = false;
			const auto rebuilt = RebuildBindings(/*forceProjection*/ false);
			changes.keybindings = rebuilt == RebuildResult::Changed;
			if (rebuilt == RebuildResult::Failed) {
				changes.keybindings = true;
				changes.engineInputContext = true;
				return changes;
			}
			REX::INFO("LiveControlMap: coalesced {} remap notification edge(s); snapshot {}", edges, rebuilt == RebuildResult::Changed ? "changed" : "unchanged");
		}
		changes.engineInputContext = RefreshActiveEngineInputContexts();
		if (!_available) changes.keybindings = changes.engineInputContext = true;
		return changes;
	}

	bool LiveControlMap::RefreshLabels(bool a_localizationChanged)
	{
		if (!_initialized || !_available) return false;
		if (a_localizationChanged) _translationCache.clear();
		// Consume queued dirty edges after this forced read to avoid a redundant rebuild.
		_seenRemapGeneration = g_remapGeneration.load(std::memory_order_acquire);
		_pendingRemapEdges = 0;
		_remapPending = false;
		return RebuildBindings(/*forceProjection*/ true) == RebuildResult::Changed;
	}
}
