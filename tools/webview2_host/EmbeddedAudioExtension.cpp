#include "EmbeddedAudioExtension.h"

#include <array>
#include <cstdint>

namespace osfui::webview2
{
    namespace
    {
        const unsigned char kManifest[] = {
#include "manifest.json.h"
        };
        const unsigned char kCollectorHtml[] = {
#include "collector.html.h"
        };
        const unsigned char kCollectorScript[] = {
#include "collector.js.h"
        };

        template <std::size_t N>
        std::string_view Bytes(const unsigned char (&data)[N]) noexcept
        {
            static_assert(N > 0);
            // utils.bin2c appends a NUL byte that is not part of the source file.
            return {reinterpret_cast<const char*>(data), N - 1};
        }

        const std::array kAssets{
            EmbeddedAudioAsset{"manifest.json", Bytes(kManifest)},
            EmbeddedAudioAsset{"collector.html", Bytes(kCollectorHtml)},
            EmbeddedAudioAsset{"collector.js", Bytes(kCollectorScript)},
        };
    }

    std::span<const EmbeddedAudioAsset> GetEmbeddedAudioExtension() noexcept
    {
        return kAssets;
    }

    const std::string& EmbeddedAudioExtensionContentId()
    {
        static const auto identity = [] {
            std::uint64_t hash = 14695981039346656037ULL;
            const auto append = [&](unsigned char byte) {
                hash = (hash ^ byte) * 1099511628211ULL;
            };
            for (const auto& asset : kAssets) {
                for (const auto byte : asset.name) append(static_cast<unsigned char>(byte));
                append(0);
                const auto size = static_cast<std::uint64_t>(asset.bytes.size());
                for (unsigned shift = 0; shift < 64; shift += 8)
                    append(static_cast<unsigned char>(size >> shift));
                for (const auto byte : asset.bytes) append(static_cast<unsigned char>(byte));
            }
            constexpr std::string_view digits = "0123456789abcdef";
            std::string result(16, '0');
            for (unsigned index = 0; index < 16; ++index)
                result[15 - index] = digits[(hash >> (index * 4)) & 0xf];
            return result;
        }();
        return identity;
    }
}
