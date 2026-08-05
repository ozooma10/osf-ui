#pragma once

#include "runtime/ControlMapPolicy.h"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace OSFUI
{
	// Version-gated, game-thread-only view of Starfield's live ControlMap. All
	// engine pointers and pooled strings are copied into owned values before a
	// snapshot is published to the rest of OSF UI.
	class LiveControlMap
	{
	public:
		struct ConflictBinding
		{
			std::string event;
			std::string title;
			std::string context;
			std::string slot;
			std::uint32_t code{ 0 };
			ControlMapPolicy::Classification classification{ ControlMapPolicy::Classification::Unknown };
			GameplayModeMask definiteModes{ 0 };
			GameplayModeMask possibleModes{ 0 };
		};

		struct Changes
		{
			bool keybindings{ false };
			bool inputContext{ false };
		};

		// First main-thread Tick after kPostDataLoad. A failure is durable and
		// fail-closed: state remains available:false and no vanilla conflict claims
		// are made. Initialized() becomes true only after the snapshot is complete.
		void Initialize();
		// Per main-thread tick. Coalesces repeated remap events into one rebuild and
		// samples the small active-context stack without re-enumerating bindings.
		[[nodiscard]] Changes Pump();
		// Layout/locale changed on the main thread: rebuild the physical projection.
		// Locale changes explicitly clear the persistent engine-translation cache;
		// keyboard-layout changes retain it because only VK -> scan projection moves.
		[[nodiscard]] bool RefreshLabels(bool a_localizationChanged = false);

		[[nodiscard]] bool Available() const { return _available; }
		[[nodiscard]] bool Initialized() const { return _initialized; }
		[[nodiscard]] std::optional<GameplayMode> CurrentMode() const { return _mode; }
		[[nodiscard]] const std::vector<ConflictBinding>& ConflictBindings() const { return _conflicts; }
		[[nodiscard]] const nlohmann::json& KeybindingsState() const { return _keybindingsState; }
		[[nodiscard]] const nlohmann::json& InputContextState() const { return _inputContextState; }
		[[nodiscard]] const std::string& FailureReason() const { return _failureReason; }
		[[nodiscard]] const std::string& GameVersion() const { return _gameVersion; }

	private:
		enum class RebuildResult : std::uint8_t
		{
			Failed,
			Unchanged,
			Changed,
		};
		RebuildResult RebuildBindings(bool a_forceProjection);
		bool RefreshActiveContexts();
		bool InstallRemapObserver();
		void Fail(std::string a_reason);
		void EncodeUnavailableStates();

		bool _initialized{ false };
		bool _available{ false };
		std::uint64_t _revision{ 0 };
		std::uint64_t _contextRevision{ 0 };
		std::uint64_t _seenRemapGeneration{ 0 };
		std::uint64_t _bindingHash{ 0 };
		std::uint64_t _pendingRemapEdges{ 0 };
		bool _remapPending{ false };
		std::chrono::steady_clock::time_point _firstRemapEdge{};
		std::chrono::steady_clock::time_point _lastRemapEdge{};
		std::string _gameVersion;
		std::string _failureReason;
		std::vector<ConflictBinding> _conflicts;
		std::unordered_map<std::string, std::string> _translationCache;
		std::array<std::string, 0x51> _contextNames;
		std::uintptr_t _controlMapAddress{ 0 };
		std::uintptr_t _validatedActiveData{ 0 };
		std::size_t _validatedActiveBytes{ 0 };
		std::vector<std::uint8_t> _activeContexts;
		std::optional<GameplayMode> _mode;
		nlohmann::json _keybindingsState{ nlohmann::json::object() };
		nlohmann::json _inputContextState{ nlohmann::json::object() };
	};
}
