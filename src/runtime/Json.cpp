#include "runtime/Json.h"

namespace OSFUI::Json
{
	std::optional<Value> Parse(std::string_view a_text, std::string_view a_sourceName)
	{
		Value parsed = Value::parse(a_text, /*cb=*/nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
		if (parsed.is_discarded()) {
			REX::ERROR("Json: failed to parse {}", a_sourceName);
			return std::nullopt;
		}
		return parsed;
	}

	std::optional<Value> ParseFile(const std::filesystem::path& a_path)
	{
		std::ifstream stream(a_path, std::ios::binary);
		if (!stream) {
			REX::WARN("Json: cannot open {}", a_path.string());
			return std::nullopt;
		}
		std::string text(std::istreambuf_iterator<char>(stream), {});
		return Parse(text, a_path.string());
	}

	std::string GetString(const Value& a_obj, std::string_view a_key, std::string_view a_default)
	{
		if (const auto it = a_obj.find(a_key); it != a_obj.end() && it->is_string()) {
			return it->get<std::string>();
		}
		return std::string(a_default);
	}

	bool GetBool(const Value& a_obj, std::string_view a_key, bool a_default)
	{
		if (const auto it = a_obj.find(a_key); it != a_obj.end() && it->is_boolean()) {
			return it->get<bool>();
		}
		return a_default;
	}

	std::int64_t GetInt(const Value& a_obj, std::string_view a_key, std::int64_t a_default)
	{
		if (const auto it = a_obj.find(a_key); it != a_obj.end() && it->is_number_integer()) {
			return it->get<std::int64_t>();
		}
		return a_default;
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
