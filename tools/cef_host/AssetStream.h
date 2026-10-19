#pragma once
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace osfui::cefhost
{
    struct AssetRange { int status{200}; std::int64_t offset{}, length{}; };

    inline AssetRange SelectAssetRange(std::string_view header, std::int64_t size)
    {
        const AssetRange full{200, 0, size}, unsatisfied{416, 0, 0};
        // Unsupported units, malformed and multiple ranges may be ignored (full response).
        if (!header.starts_with("bytes=") || header.find(',') != header.npos) return full;
        header.remove_prefix(6);
        const auto dash = header.find('-');
        if (dash == header.npos) return full;
        const auto first = header.substr(0, dash), last = header.substr(dash + 1);
        const auto number = [](std::string_view text, std::uint64_t& value) {
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
            return !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size();
        };
        std::uint64_t begin{}, end{};
        if (first.empty()) {
            if (!number(last, end)) return full;
            if (!end || !size) return unsatisfied;
            const auto count = static_cast<std::int64_t>((std::min)(end, static_cast<std::uint64_t>(size)));
            return {206, size - count, count};
        }
        if (!number(first, begin) || (!last.empty() && !number(last, end))) return full;
        if (begin >= static_cast<std::uint64_t>(size) || (!last.empty() && end < begin)) return unsatisfied;
        if (last.empty()) end = size - 1;
        end = (std::min)(end, static_cast<std::uint64_t>(size - 1));
        return {206, static_cast<std::int64_t>(begin), static_cast<std::int64_t>(end - begin + 1)};
    }

    // Stream only the requested span, including files larger than memory and HEAD without a body.
    class AssetStream
    {
    public:
        bool Open(const std::filesystem::path& path, bool head, std::string_view range)
        {
            file_.open(path, std::ios::binary | std::ios::ate);
            if (!file_) return false;
            size = file_.tellg();
            if (size < 0) return false;
            response = SelectAssetRange(head ? std::string_view{} : range, size);
            remaining_ = head ? 0 : response.length;
            file_.seekg(response.offset);
            return bool(file_);
        }
        int Read(void* data, int capacity)
        {
            if (capacity <= 0 || remaining_ == 0) return 0;
            const auto count = (std::min)(remaining_, static_cast<std::int64_t>(capacity));
            file_.read(static_cast<char*>(data), count);
            const auto read = file_.gcount();
            if (!read) return -2; // file truncated or I/O failed before the promised content length
            remaining_ -= read;
            return static_cast<int>(read);
        }
        std::int64_t Skip(std::int64_t count)
        {
            if (count < 0) return -2;
            count = (std::min)(count, remaining_);
            file_.seekg(count, std::ios::cur);
            if (!file_) return -2;
            remaining_ -= count;
            return count;
        }
        std::int64_t Remaining() const { return remaining_; }
        std::int64_t size{};
        AssetRange response;
    private:
        std::ifstream file_;
        std::int64_t remaining_{};
    };
}
