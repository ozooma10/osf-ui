#pragma once

#include "BrowserAudio.h"
#include "Win32Util.h"
#include <wrl.h>
#include <WebView2.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace osfui::webview2
{
    inline constexpr wchar_t kAudioExtensionId[] = L"nbligbkobhajkimckkfnadnghkimjcld";
    // Extract the embedded extension into an immutable, content-addressed directory.
    // Existing files must match the embedded bytes; never load modified cache files.
    std::filesystem::path PrepareAudioExtension(const std::filesystem::path& userData);

    // STA-owned tab capture. Only the reserved browser TV views use this route.
    class SpatialAudio
    {
    public:
        using Log = std::function<void(const std::string&)>;
        SpatialAudio(ICoreWebView2* source, Log log);
        ~SpatialAudio();
        SpatialAudio(const SpatialAudio&) = delete;
        SpatialAudio& operator=(const SpatialAudio&) = delete;
        bool Guarded() const;
        void Start(ICoreWebView2Environment* environment, HWND parent, bool composition = false);
        void Update(const nlohmann::json& scene, std::uint64_t expires);
        void Tick();
        void Fail(const std::string& reason);
        void SetMuted(bool muted);
        void SetLoading(bool loading);
        bool Muted() const;
        nlohmann::json Status(bool includeSpatial = false) const;

    private:
        struct State;
        std::shared_ptr<State> state;
    };
}
