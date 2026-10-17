#pragma once

#include <optional>
#include <string_view>

namespace osfui::browser
{
    enum class Backend { WebView2, Cef };

    constexpr std::string_view BackendName(Backend backend)
    {
        return backend == Backend::Cef ? "cef" : "webview2";
    }

    constexpr std::optional<Backend> ParseBackend(std::string_view name)
    {
        if (name == "webview2") return Backend::WebView2;
        if (name == "cef") return Backend::Cef;
        return std::nullopt;
    }
}
