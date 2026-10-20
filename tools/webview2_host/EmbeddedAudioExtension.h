#pragma once

#include <span>
#include <string>
#include <string_view>

namespace osfui::webview2
{
    struct EmbeddedAudioAsset
    {
        std::string_view name;
        std::string_view bytes;
    };

    [[nodiscard]] std::span<const EmbeddedAudioAsset> GetEmbeddedAudioExtension() noexcept;
    // Cache identity only; callers still verify every installed byte before use.
    [[nodiscard]] const std::string& EmbeddedAudioExtensionContentId();
}
