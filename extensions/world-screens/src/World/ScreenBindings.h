#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace RE { class NiAVObject; class TESObjectREFR; }
namespace OSFUI::ScreenBindings
{
    // Tokens are process-unique and session-scoped. Every admitted base has
    // passed the supported-model check before its ID reaches this registry.
    void Enable(std::vector<std::string> a_views, std::vector<std::uint32_t> a_bases);
    // Private fixture entry point, including vanilla DisplayScreen5 references.
    void Enable(std::vector<std::string> a_views, std::uint32_t a_dynamicBase = 0);
    std::uint32_t Bind(std::uint32_t a_ref, std::string_view a_view);
    bool Unbind(std::uint32_t a_token);
    void Reset();
    // Required identity barriers: delete, remap (invalidate both IDs), and load
    // begin. Callers must install the gated event sinks before enabling binding.
    void Invalidate(std::uint32_t a_ref);
    void BeginSessionReplacement();
    void EndSessionReplacement();
    std::uint32_t Token(std::uint32_t a_ref);
    struct Selection
    {
        std::uint32_t reference{};
        std::string view;
        bool ready{};
    };
    std::optional<Selection> Resolve(std::uint32_t a_token);
    bool HasBindings(std::string_view a_view);
    void Tick();
    // Borrowed solely inside original material setup. a_cachedSource is
    // supplied by the scoped model-load call; null on a queued appearance update.
    void OnSetup(RE::TESObjectREFR* a_ref, RE::NiAVObject* a_root, RE::NiAVObject* a_cachedSource);
}
