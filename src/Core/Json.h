#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace OSFUI::Json
{
	using Value = nlohmann::json;

	// dump() that doesnt throw
	[[nodiscard]] inline std::string Dump(const Value& a_value, int a_indent = -1)
	{
		return a_value.dump(a_indent, ' ', /*ensure_ascii=*/false, Value::error_handler_t::replace);
	}

	// Parses text (comments allowed); std::nullopt on malformed input.
	[[nodiscard]] inline std::optional<Value> Parse(std::string_view a_text)
	{
		Value parsed = Value::parse(a_text, /*cb=*/nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
		if (parsed.is_discarded()) { return std::nullopt; }
		return parsed;
	}

	// Reads and parses a file. Returns std::nullopt on a missing file or bad JSON.
	[[nodiscard]] std::optional<Value> ParseFile(const std::filesystem::path& a_path, std::string* a_outError = nullptr);

	// One lenient typed read. A missing key, a non-object receiver, or a value  of the wrong type all yield a_default; nothing here throws or rejects.
	template <class T> requires std::is_arithmetic_v<std::decay_t<T>> || std::is_convertible_v<std::decay_t<T>, std::string_view>
	[[nodiscard]] auto Get(const Value& a_obj, std::string_view a_key, T&& a_default)
	{
		using D = std::decay_t<T>;
		using R = std::conditional_t<std::is_convertible_v<D, std::string_view>, std::string, D>;

		const auto fallback = [&] { return R(std::forward<T>(a_default)); };

		const auto it = a_obj.find(a_key);
		if (it == a_obj.end()) {
			return fallback();
		}
		if constexpr (std::is_same_v<R, std::string>) {
			return it->is_string() ? it->get<std::string>() : fallback();
		} else if constexpr (std::is_same_v<D, bool>) {
			return it->is_boolean() ? it->get<bool>() : fallback();
		} else if constexpr (std::is_floating_point_v<D>) {
			return it->is_number() ? static_cast<R>(it->get<double>()) : fallback();
		} else {
			if (!it->is_number_integer()) {
				return fallback();
			}
			if constexpr (std::is_unsigned_v<D>) {
				if (!it->is_number_unsigned() && it->get<std::int64_t>() < 0) {
					return fallback();
				}
				return static_cast<R>(it->get<std::uint64_t>());
			} else {
				return static_cast<R>(it->get<std::int64_t>());
			}
		}
	}

	[[nodiscard]] inline const Value* GetArray(const Value& a_obj, std::string_view a_key)
	{
		const auto it = a_obj.find(a_key);
		return it != a_obj.end() && it->is_array() ? &*it : nullptr;
	}

	[[nodiscard]] inline const Value* GetObject(const Value& a_obj, std::string_view a_key)
	{
		const auto it = a_obj.find(a_key);
		return it != a_obj.end() && it->is_object() ? &*it : nullptr;
	}

	[[nodiscard]] inline std::vector<std::string> GetStringArray(const Value& a_obj, std::string_view a_key)
	{
		std::vector<std::string> out;
		if (const auto it = a_obj.find(a_key); it != a_obj.end() && it->is_array()) {
			for (const auto& elem : *it) {
				if (elem.is_string()) {
					out.push_back(elem.get<std::string>());
				}
			}
		}
		return out;
	}

	void ReportUnknownKeys(const Value& a_obj, std::initializer_list<std::string_view> a_known, std::string_view a_sourceName, bool a_warn);

	void CheckFormatVersion(const Value& a_obj, std::string_view a_key, std::int64_t a_known, std::string_view a_sourceName);
}
