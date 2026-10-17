#pragma once
#include <filesystem>
#include <optional>
#include <string_view>

namespace osfui::cefhost
{
    // Receives a decoded URL path. Never interpret Windows separators, ADS, or dot segments.
    inline bool SafeAssetPath(std::wstring_view path)
    {
        if (path.empty() || path.front() != L'/' || path.find_first_of(L"\\:%") != path.npos ||
            path.find(L'\0') != path.npos) return false;
        for (std::size_t start = 1; start < path.size();) {
            const auto end = path.find(L'/', start);
            const auto part = path.substr(start, end == path.npos ? path.npos : end - start);
            if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ') return false;
            for (const auto ch : part) if (ch < 32) return false;
            if (end == path.npos) break;
            start = end + 1;
        }
        return true;
    }
    inline std::optional<std::filesystem::path> ResolveAsset(const std::filesystem::path& root, std::wstring_view decoded)
    {
        if (!SafeAssetPath(decoded)) return std::nullopt;
        std::error_code ec;
        const auto base = std::filesystem::canonical(root, ec);
        if (ec) return std::nullopt;
        const auto candidate = std::filesystem::canonical(base / std::filesystem::path(decoded.substr(1)), ec);
        if (ec || !std::filesystem::is_regular_file(candidate, ec)) return std::nullopt;
        // Component comparison also rejects junction/symlink escapes from the prepared asset tree.
        auto at = candidate.begin();
        for (auto part = base.begin(); part != base.end(); ++part, ++at) {
            if (at == candidate.end() || *at != *part) return std::nullopt;
        }
        return candidate;
    }
}
