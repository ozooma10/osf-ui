#include "runtime/LiveControlMap.h"

#include "core/Log.h"
#include "input/KeyNames.h"

#include "RE/IDs_VTABLE.h"
#include "REL/ASM.h"
#include "REL/Relocation.h"
#include "REL/Trampoline.h"
#include "REX/FModule.h"

#include <Windows.h>

#ifdef ERROR
#	undef ERROR
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <tuple>
#include <type_traits>
#include <unordered_map>

namespace OSFUI
{
	namespace
	{
		constexpr REL::Version kSupportedRuntime{ 1, 16, 244, 0 };
		constexpr REL::ID kControlMapSingletonPtr{ 938003 };
		constexpr REL::ID kScaleformManagerPtr{ 938002 };
		constexpr REL::ID kContextNameTable{ 360965 };
		constexpr REL::ID kTranslateWideString{ 130928 };
		constexpr REL::ID kControlsRemappedDispatch{ 88944 };

		constexpr std::size_t kControlMapSize = 0x3A0;
		constexpr std::size_t kContextSlotsOffset = 0x10;
		constexpr std::size_t kContextCount = 0x4E;
		constexpr std::size_t kContextNameCount = 0x51;
		constexpr std::size_t kActiveContextsOffset = 0x2A8;
		constexpr std::size_t kActiveContextStride = 0x10;
		constexpr std::size_t kMappingStride = 0x28;
		constexpr std::size_t kMaxMappingsPerDevice = 4096;
		constexpr auto kRemapQuietPeriod = std::chrono::milliseconds(150);
		constexpr auto kRemapMaximumDelay = std::chrono::seconds(1);

		constexpr std::array<std::uint8_t, 58> kPreferredContextOrder{
			0x00,
			0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x4D,
			0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
			0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20,
			0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33, 0x34,
			0x3F, 0x40, 0x41, 0x42, 0x43,
			0x35, 0x36, 0x37, 0x38,
			0x49
		};

		constexpr std::array<std::uint8_t, 6> kRemapDispatchPrologue{
			0x40, 0x53, 0x48, 0x83, 0xEC, 0x40
		};

		struct ArrayHeader
		{
			std::uint32_t size;
			std::uint32_t capacity;
			std::uintptr_t data;
		};
		static_assert(sizeof(ArrayHeader) == 0x10);

		struct MappingRecord
		{
			std::uintptr_t eventEntry;
			std::uint32_t keyCode;
			std::uint32_t modifierCode;
			std::uint8_t bindingSlot;
			std::uint8_t unk11;
			std::uint16_t unk12;
			std::uint8_t sortIndex;
			std::uint8_t unk15[3];
			std::uint32_t flags;
			std::uint8_t metadata;
			std::uint8_t visible;
			std::uint8_t bakedDefaultUnbound;
			std::uint8_t unk1F;
			std::uint8_t required;
			std::uint8_t pad21[7];
		};
		static_assert(sizeof(MappingRecord) == kMappingStride);

		struct RawBinding
		{
			std::uint8_t slot{};
			std::uint32_t keyCode{};
			std::uint32_t modifierCode{};
		};

		struct GroupedRow
		{
			std::uint8_t contextId{};
			std::uint8_t sortIndex{ 0xFF };
			bool required{ false };
			std::string event;
			std::vector<RawBinding> keyboard;
		};

		using RemapDispatchFn = void (*)(const void*);
		std::atomic<std::uintptr_t> g_remapGateway{ 0 };
		std::atomic<std::uint64_t> g_remapGeneration{ 0 };

		bool IsReadableRange(std::uintptr_t a_address, std::size_t a_size)
		{
			if (!a_address || a_size == 0 || a_address + a_size < a_address) return false;
			const auto end = a_address + a_size;
			while (a_address < end) {
				MEMORY_BASIC_INFORMATION info{};
				if (::VirtualQuery(reinterpret_cast<const void*>(a_address), &info, sizeof(info)) != sizeof(info) ||
					info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
					return false;
				}
				const auto next = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
				if (next <= a_address) return false;
				a_address = next;
			}
			return true;
		}

		template <class T>
		bool SafeRead(std::uintptr_t a_address, T& a_out)
		{
			if (!IsReadableRange(a_address, sizeof(T))) return false;
			std::memcpy(std::addressof(a_out), reinterpret_cast<const void*>(a_address), sizeof(T));
			return true;
		}

		// Production snapshot reads use one SEH fault boundary around each bounded
		// copy. The investigation probe used VirtualQuery for every field/character,
		// which is intentionally conservative but stalls Starfield for seconds.
		// These leaf helpers contain no C++ objects requiring unwinding.
		template <class T>
		bool GuardedRead(std::uintptr_t a_address, T& a_out)
		{
			if (!a_address) return false;
			__try {
				std::memcpy(std::addressof(a_out), reinterpret_cast<const void*>(a_address), sizeof(T));
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool GuardedCopy(std::uintptr_t a_address, void* a_out, std::size_t a_size)
		{
			if (a_size == 0) return true;
			if (!a_address || !a_out || a_address + a_size < a_address) return false;
			__try {
				std::memcpy(a_out, reinterpret_cast<const void*>(a_address), a_size);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool GuardedCString(std::uintptr_t a_address, char* a_out, std::size_t a_capacity, std::size_t& a_length)
		{
			if (!a_address || !a_out || a_capacity == 0) return false;
			__try {
				const auto* source = reinterpret_cast<const char*>(a_address);
				for (std::size_t i = 0; i < a_capacity; ++i) {
					a_out[i] = source[i];
					if (source[i] == '\0') {
						a_length = i;
						return i != 0;
					}
				}
				return false;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		std::uintptr_t ReadRelocatedPointer(REL::ID a_id)
		{
			REL::Relocation<std::uintptr_t*> slot{ a_id };
			std::uintptr_t value = 0;
			return SafeRead(slot.address(), value) ? value : 0;
		}

		std::optional<std::string> ReadPooledString(std::uintptr_t a_entry)
		{
			for (std::size_t depth = 0; a_entry && depth < 8; ++depth) {
				std::uint8_t flags = 0;
				if (!GuardedRead(a_entry + 0x14, flags)) return std::nullopt;
				if ((flags & 0x02) != 0) {
					if (!GuardedRead(a_entry + 0x08, a_entry)) return std::nullopt;
					continue;
				}
				std::uint32_t length = 0;
				if (!GuardedRead(a_entry + 0x08, length) || length == 0 || length > 256) return std::nullopt;
				std::string out(length, '\0');
				if (!GuardedCopy(a_entry + 0x18, out.data(), length)) return std::nullopt;
				for (const auto ch : out) {
					if (static_cast<unsigned char>(ch) < 0x20 || ch == '|' || ch == '\x7F') return std::nullopt;
				}
				return out;
			}
			return std::nullopt;
		}

		bool SnapshotContextNames(std::array<std::string, kContextNameCount>& a_names)
		{
			REL::Relocation<std::uintptr_t> table{ kContextNameTable };
			if (!IsReadableRange(table.address(), kContextNameCount * sizeof(std::uintptr_t))) return false;
			const auto* entries = reinterpret_cast<const std::uintptr_t*>(table.address());
			for (std::size_t id = 0; id < kContextNameCount; ++id) {
				const auto text = entries[id];
				if (!text) continue;
				std::array<char, 128> chars{};
				std::size_t length = 0;
				if (!GuardedCString(text, chars.data(), chars.size(), length)) return false;
				for (std::size_t i = 0; i < length; ++i) {
					if (static_cast<unsigned char>(chars[i]) < 0x20 || chars[i] == '\x7F') return false;
				}
				a_names[id].assign(chars.data(), length);
			}
			return true;
		}

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
				HashInteger(hash, row.contextId);
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

		std::uint32_t ContextOrder(std::uint8_t a_contextId)
		{
			const auto found = std::ranges::find(kPreferredContextOrder, a_contextId);
			return found == kPreferredContextOrder.end() ?
				static_cast<std::uint32_t>(kPreferredContextOrder.size()) + a_contextId :
				static_cast<std::uint32_t>(std::distance(kPreferredContextOrder.begin(), found));
		}

		std::uint8_t CategoryOwner(std::uint8_t a_contextId)
		{
			if (a_contextId >= 0x06 && a_contextId <= 0x0A) return 0x06;
			if (a_contextId >= 0x02 && a_contextId <= 0x05) return 0x02;
			if (a_contextId == 0x21 || a_contextId == 0x22) return 0x21;
			if (a_contextId == 0x26 || a_contextId == 0x27) return 0x26;
			return a_contextId;
		}

		std::uint32_t VirtualKeyToScan(std::uint32_t a_code)
		{
			if (a_code == 0xFF || a_code == 0x7FFFFFFF) return 0;
			if (a_code == VK_PAUSE) return 0xC5;
			if (a_code == VK_NUMLOCK) return 0x45;
			if (a_code == VK_SNAPSHOT) return 0xB7;
			const auto scan = ::MapVirtualKeyExW(a_code, MAPVK_VK_TO_VSC_EX, ::GetKeyboardLayout(0));
			if (!scan) return 0;
			const auto prefix = (scan >> 8) & 0xFF;
			const auto set1 = scan & 0xFF;
			return prefix == 0xE0 || prefix == 0xE1 ? (set1 | 0x80) : set1;
		}

		std::wstring Utf8ToWide(std::string_view a_text)
		{
			const auto size = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a_text.data(),
				static_cast<int>(a_text.size()), nullptr, 0);
			if (size <= 0) return {};
			std::wstring out(static_cast<std::size_t>(size), L'\0');
			(void)::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a_text.data(),
				static_cast<int>(a_text.size()), out.data(), size);
			return out;
		}

		std::string WideToUtf8(std::wstring_view a_text)
		{
			const auto size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_text.data(),
				static_cast<int>(a_text.size()), nullptr, 0, nullptr, nullptr);
			if (size <= 0) return {};
			std::string out(static_cast<std::size_t>(size), '\0');
			(void)::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_text.data(),
				static_cast<int>(a_text.size()), out.data(), size, nullptr, nullptr);
			return out;
		}

		std::uintptr_t ResolveTranslator()
		{
			const auto manager = ReadRelocatedPointer(kScaleformManagerPtr);
			std::uintptr_t wrapper = 0, translator = 0, vtable = 0;
			if (!manager || !SafeRead(manager + 0x20, wrapper) || !wrapper ||
				!SafeRead(wrapper, translator) || !translator || !SafeRead(translator, vtable) ||
				vtable != RE::VTABLE::BSScaleformTranslator__ScaleformImpl[0].address()) {
				return 0;
			}
			return translator;
		}

		std::string Translate(std::uintptr_t a_translator, std::string_view a_token)
		{
			if (!a_translator) return std::string(a_token);
			const auto wide = Utf8ToWide(a_token);
			if (wide.empty()) return std::string(a_token);
			using Fn = RE::BSFixedStringW* (*)(void*, RE::BSFixedStringW*, const wchar_t*);
			REL::Relocation<Fn> translate{ kTranslateWideString };
			RE::BSFixedStringW result;
			translate(reinterpret_cast<void*>(a_translator), std::addressof(result), wide.c_str());
			return result.data() ? WideToUtf8({ result.data(), result.length() }) : std::string(a_token);
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
		_inputContextState = {
			{ "available", false }, { "revision", _contextRevision }, { "mode", nullptr },
			{ "contexts", nlohmann::json::array() },
		};
	}

	void LiveControlMap::Fail(std::string a_reason)
	{
		_available = false;
		_failureReason = std::move(a_reason);
		_conflicts.clear();
		_controlMapAddress = 0;
		_validatedActiveData = 0;
		_validatedActiveBytes = 0;
		_remapPending = false;
		_pendingRemapEdges = 0;
		_activeContexts.clear();
		_mode.reset();
		++_revision;
		++_contextRevision;
		EncodeUnavailableStates();
		REX::ERROR("LiveControlMap: unavailable -- {} (Starfield {})", _failureReason, _gameVersion);
	}

	void LiveControlMap::Initialize()
	{
		if (_initialized) return;
		const auto game = REX::FModule::GetLoadedModule("Starfield.exe");
		const auto version = game.GetFileVersion();
		_gameVersion = version.string();
		if (version != kSupportedRuntime) {
			Fail(std::format("unsupported executable; live control-map layout is proven only for {}", kSupportedRuntime));
			_initialized = true;
			return;
		}
		if (RebuildBindings(/*forceProjection*/ true) == RebuildResult::Failed || !RefreshActiveContexts()) {
			_initialized = true;
			return;
		}
		if (!InstallRemapObserver()) {
			Fail("ControlsRemappedEvent observer failed its hook-site safety gate");
			_initialized = true;
			return;
		}
		_seenRemapGeneration = g_remapGeneration.load(std::memory_order_acquire);
		_initialized = true;
		REX::INFO("LiveControlMap: ready -- {} visible actions, revision {}, mode {}", _keybindingsState["actions"].size(),
			_revision, _mode ? GameplayModeName(*_mode) : "unknown");
	}

	bool LiveControlMap::InstallRemapObserver()
	{
		if (g_remapGateway.load(std::memory_order_acquire)) return true;
		REL::Relocation<std::uintptr_t> target{ kControlsRemappedDispatch };
		std::array<std::uint8_t, kRemapDispatchPrologue.size()> actual{};
		if (!IsReadableRange(target.address(), actual.size())) return false;
		std::memcpy(actual.data(), reinterpret_cast<const void*>(target.address()), actual.size());
		if (actual != kRemapDispatchPrologue) return false;
		auto& trampoline = REL::GetTrampoline();
		constexpr auto copied = kRemapDispatchPrologue.size();
		if (trampoline.empty() || trampoline.free_size() < copied + 2 * sizeof(REL::ASM::JMP14)) return false;
		auto* gateway = static_cast<std::byte*>(trampoline.allocate(copied + sizeof(REL::ASM::JMP14)));
		std::memcpy(gateway, reinterpret_cast<const void*>(target.address()), copied);
		const REL::ASM::JMP14 jumpBack{ target.address() + copied };
		std::memcpy(gateway + copied, std::addressof(jumpBack), sizeof(jumpBack));
		g_remapGateway.store(reinterpret_cast<std::uintptr_t>(gateway), std::memory_order_release);
		target.write_jmp<5>(RemapDispatchHook);
		REX::INFO("LiveControlMap: observing ControlsRemappedEvent dispatch (Address Library ID 88944)");
		return true;
	}

	LiveControlMap::RebuildResult LiveControlMap::RebuildBindings(bool a_forceProjection)
	{
		using Clock = std::chrono::steady_clock;
		const auto started = Clock::now();
		const auto millis = [](Clock::time_point a_begin, Clock::time_point a_end) {
			return std::chrono::duration_cast<std::chrono::milliseconds>(a_end - a_begin).count();
		};
		const auto controlMap = ReadRelocatedPointer(kControlMapSingletonPtr);
		if (!controlMap || !IsReadableRange(controlMap, kControlMapSize) ||
			*reinterpret_cast<const std::uintptr_t*>(controlMap) != RE::VTABLE::ControlMap[0].address()) {
			Fail("ControlMap singleton or vtable failed validation");
			return RebuildResult::Failed;
		}
		std::array<std::string, kContextNameCount> contextNames;
		if (!SnapshotContextNames(contextNames) || contextNames[0x00] != "MainGameplay" || contextNames[0x49] != "Vehicle") {
			Fail("input-context name table failed validation");
			return RebuildResult::Failed;
		}
		const auto validated = Clock::now();

		std::map<std::pair<std::uint8_t, std::string>, GroupedRow> grouped;
		std::unordered_map<std::uintptr_t, std::string> stringCache;
		const auto* contextSlots = reinterpret_cast<const std::uintptr_t*>(controlMap + kContextSlotsOffset);
		for (std::uint8_t contextId = 0; contextId < kContextCount; ++contextId) {
			const auto context = contextSlots[contextId];
			if (!context) continue;
			std::array<ArrayHeader, 2> deviceHeaders{};
			if (!GuardedCopy(context, deviceHeaders.data(), sizeof(deviceHeaders))) {
				Fail("ControlMap device mapping headers were unreadable");
				return RebuildResult::Failed;
			}
			for (std::uint8_t device = 0; device < 2; ++device) {
				const auto header = deviceHeaders[device];
				if (header.size > header.capacity || header.size > kMaxMappingsPerDevice ||
					(header.size && !header.data)) {
					Fail("ControlMap device mapping array failed shape validation");
					return RebuildResult::Failed;
				}
				std::vector<MappingRecord> mappings(header.size);
				if (header.size && !GuardedCopy(header.data, mappings.data(), mappings.size() * sizeof(MappingRecord))) {
					Fail("ControlMap device mapping array was unreadable");
					return RebuildResult::Failed;
				}
				for (const auto& mapping : mappings) {
					if (!mapping.visible) continue;
					auto found = stringCache.find(mapping.eventEntry);
					if (found == stringCache.end()) {
						const auto event = ReadPooledString(mapping.eventEntry);
						if (!event) {
							Fail("ControlMap event string failed pooled-string validation");
							return RebuildResult::Failed;
						}
						found = stringCache.emplace(mapping.eventEntry, *event).first;
					}
					auto& row = grouped[{ contextId, found->second }];
					row.contextId = contextId;
					row.event = found->second;
					row.sortIndex = (std::min)(row.sortIndex, mapping.sortIndex);
					row.required = row.required || mapping.required != 0;
					if (device == 0 && mapping.bindingSlot < 2) {
						row.keyboard.push_back({ mapping.bindingSlot, mapping.keyCode, mapping.modifierCode });
					}
				}
			}
		}
		if (grouped.size() < 100 || grouped.size() > 1000) {
			Fail(std::format("visible action count {} is outside the validated range", grouped.size()));
			return RebuildResult::Failed;
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
			return std::tuple{ ContextOrder(a.contextId), a.sortIndex, a.event } <
			       std::tuple{ ContextOrder(b.contextId), b.sortIndex, b.event };
		});
		const auto structuralHash = HashRows(rows);
		const auto sorted = Clock::now();
		if (!a_forceProjection && _available && structuralHash == _bindingHash) {
			_contextNames = std::move(contextNames);
			if (_controlMapAddress != controlMap) {
				_controlMapAddress = controlMap;
				_validatedActiveData = 0;
				_validatedActiveBytes = 0;
			}
			_failureReason.clear();
			REX::INFO("LiveControlMap: dirty snapshot unchanged in {} ms "
				"(validate {}, enumerate {}, sort/hash {}); skipped translation, JSON, and broadcasts",
				millis(started, sorted), millis(started, validated),
				millis(validated, enumerated), millis(enumerated, sorted));
			return RebuildResult::Unchanged;
		}

		nlohmann::json actions = nlohmann::json::array();
		std::vector<ConflictBinding> conflicts;
		const auto translator = ResolveTranslator();
		std::size_t translationHits = 0;
		std::size_t translationCalls = 0;
		auto translate = [&](const std::string& a_token) -> std::string {
			if (const auto found = _translationCache.find(a_token); found != _translationCache.end()) {
				++translationHits;
				return found->second;
			}
			++translationCalls;
			auto value = Translate(translator, a_token);
			// Do not persist raw fallbacks from a transiently unavailable translator.
			if (translator) _translationCache.emplace(a_token, value);
			return value;
		};
		for (const auto& row : rows) {
			const auto& contextName = contextNames[row.contextId];
			const auto& categoryName = contextNames[CategoryOwner(row.contextId)];
			if (contextName.empty() || categoryName.empty()) {
				Fail("context name became unreadable during projection");
				return RebuildResult::Failed;
			}
			const auto policy = ControlMapPolicy::Classify(row.contextId, contextName);
			const auto baseToken = std::format("${}_{}", contextName, row.event);
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
				const auto code = VirtualKeyToScan(raw.keyCode);
				const auto modifier = VirtualKeyToScan(raw.modifierCode);
				const auto slot = raw.slot == 0 ? "main" : "alternate";
				nlohmann::json chord = nlohmann::json::array();
				if (modifier) chord.push_back(KeyName(static_cast<ScanCode>(modifier)));
				if (code) chord.push_back(KeyName(static_cast<ScanCode>(code)));
				bindings.push_back({
					{ "slot", slot }, { "key", code ? nlohmann::json(KeyName(static_cast<ScanCode>(code))) : nlohmann::json(nullptr) },
					{ "chord", std::move(chord) }, { "unbound", code == 0 },
				});
				// Vanilla chords are display-only. Single-key main and alternate
				// bindings both participate in conflict analysis.
				if (code && !modifier) {
					conflicts.push_back({ row.event, label, contextName, slot, code, policy.classification,
						policy.definiteModes, policy.possibleModes });
				}
			}

			actions.push_back({
				{ "event", row.event }, { "label", label }, { "category", category },
				{ "context", { { "id", row.contextId }, { "name", contextName }, { "order", ContextOrder(row.contextId) } } },
				{ "classification", ControlMapPolicy::ClassificationName(policy.classification) },
				{ "modes", { { "definite", EncodeModes(policy.definiteModes) }, { "possible", EncodeModes(policy.possibleModes) } } },
				{ "sortIndex", row.sortIndex }, { "required", row.required }, { "bindings", std::move(bindings) },
			});
		}

		const auto actionCount = actions.size();
		_conflicts = std::move(conflicts);
		_bindingHash = structuralHash;
		_contextNames = std::move(contextNames);
		_controlMapAddress = controlMap;
		_validatedActiveData = 0;
		_validatedActiveBytes = 0;
		_available = true;
		_failureReason.clear();
		++_revision;
		_keybindingsState = {
			{ "available", true }, { "revision", _revision }, { "gameVersion", _gameVersion },
			{ "actions", std::move(actions) },
		};
		const auto projected = Clock::now();
		REX::INFO("LiveControlMap: rebuilt {} visible actions in {} ms "
			"(validate {}, enumerate {}, sort/hash {}, project {}; translations {} engine / {} cached)",
			actionCount, millis(started, projected), millis(started, validated),
			millis(validated, enumerated), millis(enumerated, sorted), millis(sorted, projected),
			translationCalls, translationHits);
		return RebuildResult::Changed;
	}

	bool LiveControlMap::RefreshActiveContexts()
	{
		if (!_available) return false;
		if (!_controlMapAddress) {
			Fail("validated ControlMap snapshot was unavailable");
			return false;
		}
		// The singleton object was range/vtable validated by RebuildBindings.
		// Read this small game-owned header directly on the game thread. Validate
		// its backing allocation only when the pointer grows or changes, not on
		// every frame.
		const auto header = *reinterpret_cast<const ArrayHeader*>(_controlMapAddress + kActiveContextsOffset);
		if (header.size > header.capacity || header.size > 128 || (header.size && !header.data)) {
			Fail("active input-context stack failed shape validation");
			return false;
		}
		const auto activeBytes = static_cast<std::size_t>(header.size) * kActiveContextStride;
		if (header.size && (header.data != _validatedActiveData || activeBytes > _validatedActiveBytes)) {
			if (!IsReadableRange(header.data, activeBytes)) {
				Fail("active input-context storage was unreadable");
				return false;
			}
			_validatedActiveData = header.data;
			_validatedActiveBytes = activeBytes;
		}
		std::array<std::uint8_t, 128> active{};
		for (std::uint32_t i = 0; i < header.size; ++i) {
			const auto id = *reinterpret_cast<const std::uint8_t*>(
				header.data + static_cast<std::size_t>(i) * kActiveContextStride);
			if (id >= kContextNameCount) {
				Fail("active input-context entry failed validation");
				return false;
			}
			active[i] = id;
		}
		const std::span<const std::uint8_t> activeSpan{ active.data(), header.size };
		const auto mode = ControlMapPolicy::DeriveMode(activeSpan);
		if (_activeContexts.size() == header.size &&
			std::ranges::equal(_activeContexts, activeSpan) && mode == _mode && !_inputContextState.empty()) return false;
		_activeContexts.assign(activeSpan.begin(), activeSpan.end());
		_mode = mode;
		nlohmann::json contexts = nlohmann::json::array();
		for (const auto id : _activeContexts) {
			const auto& name = _contextNames[id];
			if (name.empty()) {
				Fail("active input-context name failed validation");
				return false;
			}
			contexts.push_back({ { "id", id }, { "name", name } });
		}
		++_contextRevision;
		_inputContextState = {
			{ "available", true }, { "revision", _contextRevision },
			{ "mode", _mode ? nlohmann::json(GameplayModeName(*_mode)) : nlohmann::json(nullptr) },
			{ "contexts", std::move(contexts) },
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
				changes.inputContext = true;
				return changes;
			}
			REX::INFO("LiveControlMap: coalesced {} remap notification edge(s); snapshot {}",
				edges, rebuilt == RebuildResult::Changed ? "changed" : "unchanged");
		}
		changes.inputContext = RefreshActiveContexts();
		if (!_available) changes.keybindings = changes.inputContext = true;
		return changes;
	}

	bool LiveControlMap::RefreshLabels(bool a_localizationChanged)
	{
		if (!_initialized || !_available) return false;
		if (a_localizationChanged) _translationCache.clear();
		// This forced refresh reads the newest map too, so consume any queued dirty
		// edges and avoid a redundant debounce rebuild on the following tick.
		_seenRemapGeneration = g_remapGeneration.load(std::memory_order_acquire);
		_pendingRemapEdges = 0;
		_remapPending = false;
		return RebuildBindings(/*forceProjection*/ true) == RebuildResult::Changed;
	}
}
