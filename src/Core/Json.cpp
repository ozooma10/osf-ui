#include "Core/Json.h"
#include "Core/Utf8Path.h"

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
				REX::WARN("Json: cannot open {}", Utf8Path(a_path));
			}
			return std::nullopt;
		}
		try {
			return Value::parse(stream, /*cb=*/nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
		} catch (const std::exception& e) {
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
				REX::ERROR("Json: failed to parse {} — {}", Utf8Path(a_path), what);
			}
			return std::nullopt;
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
