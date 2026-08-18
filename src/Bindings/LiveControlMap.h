#pragma once

#include "Bindings/ControlMapPolicy.h"

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
	class LiveControlMap
	{
	public:
		struct ConflictBinding
		{
			std::string event;
			std::string title;
			std::string engineInputContextName;
			std::string slot;
			std::uint32_t code{ 0 };
			ControlMapPolicy::Classification classification{ ControlMapPolicy::Classification::Unknown };
			GameplayModeMask definiteModes{ 0 };
			GameplayModeMask possibleModes{ 0 };
		};

		struct Changes
		{
			bool keybindings{ false };
			bool engineInputContext{ false };
		};

		void Initialize();
		[[nodiscard]] Changes Pump();
		[[nodiscard]] bool RefreshLabels(bool a_localizationChanged = false);

		[[nodiscard]] bool Available() const { return _available; }
		[[nodiscard]] bool Initialized() const { return _initialized; }
		[[nodiscard]] std::optional<GameplayMode> CurrentMode() const { return _mode; }
		[[nodiscard]] const std::vector<ConflictBinding>& ConflictBindings() const { return _conflicts; }
		[[nodiscard]] const nlohmann::json& KeybindingsState() const { return _keybindingsState; }
		[[nodiscard]] const nlohmann::json& EngineInputContextState() const { return _engineInputContextState; }
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
		bool RefreshActiveEngineInputContexts();
		bool InstallRemapObserver();
		void Fail(std::string a_reason);
		void EncodeUnavailableStates();

		bool _initialized{ false };
		bool _available{ false };
		std::uint64_t _revision{ 0 };
		std::uint64_t _engineInputContextRevision{ 0 };
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
		std::array<std::string, 0x51> _engineInputContextNames;
		std::uintptr_t _controlMapAddress{ 0 };
		std::uintptr_t _validatedActiveData{ 0 };
		std::size_t _validatedActiveBytes{ 0 };
		std::vector<std::uint8_t> _activeEngineInputContexts;
		std::optional<GameplayMode> _mode;
		nlohmann::json _keybindingsState{ nlohmann::json::object() };
		nlohmann::json _engineInputContextState{ nlohmann::json::object() };
	};
}
