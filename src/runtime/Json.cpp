#include "runtime/Json.h"

#include <algorithm>
#include <fstream>

namespace OSFUI::Json
{
	std::optional<Value> ParseFile(const std::filesystem::path& a_path, std::string* a_outError)
	{
		if (a_outError) {
			a_outError->clear();
		}
		std::ifstream stream(a_path, std::ios::binary);
		if (!stream) {
			if (a_outError) {
				*a_outError = "cannot open file";
			} else {
				REX::WARN("Json: cannot open {}", a_path.string());
			}
			return std::nullopt;
		}
		// Parsed WITH exceptions so the failure carries line/column: it is the
		// whole value of the reason string, both in the banner and in the log.
		try {
			return Value::parse(stream, /*cb=*/nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
		} catch (const std::exception& e) {
			// "[json.exception.parse_error.101] parse error at line 2, ..." —
			// strip the bracketed library id; the position info is the value.
			std::string_view what = e.what();
			if (!what.empty() && what.front() == '[') {
				if (const auto end = what.find("] "); end != std::string_view::npos) {
					what.remove_prefix(end + 2);
				}
			}
			if (what.empty()) {
				what = "unreadable JSON";
			}
			if (a_outError) {
				*a_outError = std::string(what);
			} else {
				REX::ERROR("Json: failed to parse {} — {}", a_path.string(), what);
			}
			return std::nullopt;
		}
	}

	void CheckFormatVersion(const Value& a_obj, std::string_view a_key, std::int64_t a_known, std::string_view a_sourceName)
	{
		if (const auto v = Get(a_obj, a_key, a_known); v > a_known) {
			REX::INFO("{} declares {} {} (this build knows {}) — written for a newer OSF UI; unknown fields are ignored",
				a_sourceName, a_key, v, a_known);
		}
	}

	void ReportUnknownKeys(const Value& a_obj, std::initializer_list<std::string_view> a_known, std::string_view a_sourceName, bool a_warn)
	{
		if (!a_obj.is_object()) {
			return;
		}
		for (const auto& [key, value] : a_obj.items()) {
			if (key.starts_with("$")) {
				continue;  // $-prefixed keys are reserved meta (stamps, editor $schema/$comment)
			}
			if (std::ranges::find(a_known, key) == a_known.end()) {
				if (a_warn) {
					REX::WARN("{}: unknown key '{}' is ignored (typo?)", a_sourceName, key.substr(0, 64));
				} else {
					REX::INFO("{}: unknown key '{}' ignored (fine if this file targets a newer OSF UI)", a_sourceName, key.substr(0, 64));
				}
			}
		}
	}
}
