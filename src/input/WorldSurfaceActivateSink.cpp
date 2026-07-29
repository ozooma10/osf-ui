#include "input/WorldSurfaceActivateSink.h"

#if defined(OSFUI_WITH_WORLD_SURFACES)

#include "core/StringUtil.h"
#include "runtime/Runtime.h"

#include "RE/P/PlayerCharacter.h"
#include "RE/T/TESDataHandler.h"
#include "RE/T/TESFile.h"
#include "RE/T/TESForm.h"

namespace OSFUI
{
	namespace
	{
		enum class PluginTier
		{
			kFull,
			kMedium,
			kSmall,
		};

		struct ResolvedPluginForm
		{
			bool                         pluginFound{ false };
			std::optional<std::uint32_t> runtimeFormId;
			std::string_view             tier;
			std::uint8_t                 compileIndex{ 0xFF };
		};

		struct ActivationBinding
		{
			std::size_t                  surfaceIndex{ 0 };
			std::optional<std::uint32_t> runtimeFormId;
			bool                         editorIdFallback{ false };
		};

		std::vector<ActivationBinding> g_bindings;

		[[nodiscard]] std::string_view FileName(const RE::TESFile& a_file)
		{
			const auto* end = std::find(
				a_file.fileName, a_file.fileName + sizeof(a_file.fileName), '\0');
			return { a_file.fileName, static_cast<std::size_t>(end - a_file.fileName) };
		}

		[[nodiscard]] std::optional<std::uint32_t> ComposeRuntimeFormId(
			const RE::TESFile& a_file, PluginTier a_tier, std::uint32_t a_localFormId)
		{
			switch (a_tier) {
			case PluginTier::kFull:
				if (a_localFormId > 0x00FFFFFF || a_file.compileIndex >= 0xFD) {
					return std::nullopt;
				}
				return (static_cast<std::uint32_t>(a_file.compileIndex) << 24) |
					a_localFormId;
			case PluginTier::kMedium:
				if (a_localFormId > 0x0000FFFF) {
					return std::nullopt;
				}
				return 0xFD000000u |
					(static_cast<std::uint32_t>(a_file.fileIndex.mediumIndex) << 16) |
					a_localFormId;
			case PluginTier::kSmall:
				if (a_localFormId > 0x00000FFF || a_file.fileIndex.smallIndex > 0x0FFF) {
					return std::nullopt;
				}
				return 0xFE000000u |
					(static_cast<std::uint32_t>(a_file.fileIndex.smallIndex) << 12) |
					a_localFormId;
			}
			return std::nullopt;
		}

		[[nodiscard]] ResolvedPluginForm ResolveFromTier(
			const auto& a_files, PluginTier a_tier, std::string_view a_tierName,
			std::string_view a_plugin, std::uint32_t a_localFormId)
		{
			for (const auto* file : a_files) {
				if (!file || !StringUtil::EqualsCaseInsensitiveAscii(FileName(*file), a_plugin)) {
					continue;
				}
				return {
					.pluginFound = true,
					.runtimeFormId = ComposeRuntimeFormId(*file, a_tier, a_localFormId),
					.tier = a_tierName,
					.compileIndex = file->compileIndex,
				};
			}
			return {};
		}

		[[nodiscard]] ResolvedPluginForm ResolvePluginLocalForm(
			const RE::TESDataHandler& a_data, std::string_view a_plugin,
			std::uint32_t a_localFormId)
		{
			const auto& files = a_data.compiledFileCollection;
			if (auto resolved = ResolveFromTier(
					files.files, PluginTier::kFull, "full", a_plugin, a_localFormId);
				resolved.pluginFound) {
				return resolved;
			}
			if (auto resolved = ResolveFromTier(
					files.mediumFiles, PluginTier::kMedium, "medium", a_plugin, a_localFormId);
				resolved.pluginFound) {
				return resolved;
			}
			return ResolveFromTier(
				files.smallFiles, PluginTier::kSmall, "small", a_plugin, a_localFormId);
		}

		[[nodiscard]] bool MatchesEditorId(
			const Config::WorldSurfaceEntry& a_entry,
			const RE::TESObjectREFR& a_target)
		{
			if (a_entry.activateEditorId.empty()) {
				return false;
			}
			if (const char* editorId = a_target.GetFormEditorID();
				editorId && *editorId && StringUtil::EqualsCaseInsensitiveAscii(
					editorId, a_entry.activateEditorId)) {
				return true;
			}
			const auto base = a_target.GetBaseObject();
			if (const char* editorId = base ? base->GetFormEditorID() : nullptr;
				editorId && *editorId) {
				return StringUtil::EqualsCaseInsensitiveAscii(
					editorId, a_entry.activateEditorId);
			}
			return false;
		}

		[[nodiscard]] bool Matches(
			const ActivationBinding& a_binding,
			const Config::WorldSurfaceEntry& a_entry,
			const RE::TESObjectREFR& a_target)
		{
			if (a_binding.runtimeFormId) {
				const auto targetId = static_cast<std::uint32_t>(a_target.GetFormID());
				const auto base = a_target.GetBaseObject();
				const auto baseId = base ? static_cast<std::uint32_t>(base->GetFormID()) : 0;
				if (targetId == *a_binding.runtimeFormId || baseId == *a_binding.runtimeFormId) {
					return true;
				}
			}
			return a_binding.editorIdFallback && MatchesEditorId(a_entry, a_target);
		}
	}

	bool WorldSurfaceActivateSink::Install()
	{
		g_bindings.clear();
		const auto& surfaces = Runtime::Get().GetConfig().worldSurfaces;
		const bool needsFormResolution = std::ranges::any_of(surfaces, [](const auto& a_entry) {
			return !a_entry.activatePlugin.empty();
		});
		const auto* data = needsFormResolution ? RE::TESDataHandler::GetSingleton() : nullptr;
		if (needsFormResolution && !data) {
			REX::WARN("WorldSurfaceActivateSink: TESDataHandler unavailable; "
				"plugin/FormID activation bindings could not resolve");
		}

		for (std::size_t i = 0; i < surfaces.size(); ++i) {
			const auto& entry = surfaces[i];
			ActivationBinding binding{
				.surfaceIndex = i,
				.editorIdFallback = !entry.activateEditorId.empty(),
			};
			if (data && !entry.activatePlugin.empty()) {
				const auto resolved = ResolvePluginLocalForm(
					*data, entry.activatePlugin, entry.activateFormId);
				if (!resolved.pluginFound) {
					REX::WARN("WorldSurfaceActivateSink: activation plugin '{}' for '{}' "
						"is not loaded; plugin/FormID binding disabled",
						entry.activatePlugin, entry.view);
				} else if (!resolved.runtimeFormId) {
					REX::WARN("WorldSurfaceActivateSink: local FormID {:#x} does not fit "
						"the loaded {} tier for '{}' (compile index {:#x}); binding disabled",
						entry.activateFormId, resolved.tier, entry.activatePlugin,
						resolved.compileIndex);
				} else if (!RE::TESForm::LookupByID(*resolved.runtimeFormId)) {
					REX::WARN("WorldSurfaceActivateSink: '{}':{:#x} composed as runtime "
						"FormID {:#010x}, but no loaded form has that ID; binding disabled",
						entry.activatePlugin, entry.activateFormId, *resolved.runtimeFormId);
				} else {
					binding.runtimeFormId = resolved.runtimeFormId;
					REX::INFO("WorldSurfaceActivateSink: '{}' local FormID {:#x} resolved "
						"to {:#010x} ({}) for '{}'",
						entry.activatePlugin, entry.activateFormId,
						*resolved.runtimeFormId, resolved.tier, entry.view);
				}
			}
			if (binding.runtimeFormId || binding.editorIdFallback) {
				g_bindings.push_back(binding);
			}
		}

		if (!g_bindings.empty()) {
			REX::INFO("WorldSurfaceActivateSink: armed {} keyboard-E activation "
				"binding(s) using PlayerCharacter::crosshairRef", g_bindings.size());
		}
		return true;
	}

	void WorldSurfaceActivateSink::OnKeyDown(
		std::uint32_t a_virtualKey, bool a_repeat)
	{
		constexpr std::uint32_t kVirtualKeyE = 'E';
		auto& runtime = Runtime::Get();
		if (a_repeat || a_virtualKey != kVirtualKeyE || runtime.IsInputCaptured() ||
			g_bindings.empty()) {
			return;
		}
		const auto* player = RE::PlayerCharacter::GetSingleton();
		const auto* target = player ? player->crosshairRef : nullptr;
		if (!target) {
			REX::INFO("WorldSurfaceActivateSink: keyboard E had no crosshair target");
			return;
		}
		const auto& surfaces = runtime.GetConfig().worldSurfaces;
		for (const auto& binding : g_bindings) {
			if (binding.surfaceIndex >= surfaces.size()) {
				continue;
			}
			const auto& entry = surfaces[binding.surfaceIndex];
			if (!Matches(binding, entry, *target)) {
				continue;
			}
			REX::INFO("WorldSurfaceActivateSink: target {:#010x} activated -> opening '{}'",
				static_cast<std::uint32_t>(target->GetFormID()), entry.view);
			runtime.EnqueueOpenView(entry.view);
			return;
		}
		const auto base = target->GetBaseObject();
		REX::INFO("WorldSurfaceActivateSink: keyboard E target {:#010x} (base {:#010x}) "
			"matched no configured world screen",
			static_cast<std::uint32_t>(target->GetFormID()),
			base ? static_cast<std::uint32_t>(base->GetFormID()) : 0);
	}
}

#endif