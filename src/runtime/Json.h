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

// JSON helpers. All parsing goes through these (no exceptions escape) and
// through typed Get*() accessors that fall back to defaults on missing keys or
// wrong types. JSON here is mod-provided content: untrusted input, never a
// reason to crash the game.
//
// Serialization has the same contract and needs Dump() to hold it:
// nlohmann's dump() defaults to error_handler_t::strict, which THROWS
// type_error.316 when a string holds an incomplete UTF-8 sequence. Most of our
// dump sites sit on the game thread's tick or a pipe writer with no handler
// above them, so a strict throw is a std::terminate. Producers bound text with
// StringUtil::TruncateUtf8 (core/StringUtil.h) so a split sequence should never
// reach here; Dump() is the backstop for the one that does.
//
// This header is shared with osfui_webview2_host.exe, which builds WITHOUT
// src/pch.h — so it must include everything it uses, and everything the host
// calls must be defined here rather than in Json.cpp. The functions that log
// (Parse, ParseFile, CheckFormatVersion, ReportUnknownKeys) stay out of line:
// REX logging is the plugin's, and a host-side call to one is a link error,
// which is the diagnostic we want.

namespace OSFUI::Json
{
	using Value = nlohmann::json;

	// dump() that substitutes U+FFFD for malformed UTF-8 instead of throwing.
	// Use this everywhere a throw would be fatal — which is nearly everywhere.
	// a_indent is forwarded verbatim (-1 = compact, as nlohmann's default).
	[[nodiscard]] inline std::string Dump(const Value& a_value, int a_indent = -1)
	{
		return a_value.dump(a_indent, ' ', /*ensure_ascii=*/false,
			Value::error_handler_t::replace);
	}

	// Parses text (comments allowed); std::nullopt on malformed input.
	//
	// Silent by design. Every caller already has a better channel for the
	// failure than a log line — a protocol fault naming the offending view, an
	// "invalid-value" result, a [content] warning naming the mod — so logging
	// here only ever double-reported. Use this for every parse: a raw
	// nlohmann::json::parse() skips ignore_comments, which is why a schema with
	// `//` comments used to load from disk yet be rejected over the C ABI.
	[[nodiscard]] inline std::optional<Value> Parse(std::string_view a_text)
	{
		Value parsed = Value::parse(a_text, /*cb=*/nullptr, /*allow_exceptions=*/false, /*ignore_comments=*/true);
		if (parsed.is_discarded()) {
			return std::nullopt;
		}
		return parsed;
	}

	// Reads and parses a file. Returns std::nullopt on a missing file or bad JSON.
	//
	// a_outError, when given, receives "cannot open file" or nlohmann's parse
	// message with line/column and is empty exactly when parsing succeeded; the
	// caller then owns severity and wording (the settings load-error banner
	// renders it). Without it the same reason is logged here instead.
	[[nodiscard]] std::optional<Value> ParseFile(const std::filesystem::path& a_path,
		std::string* a_outError = nullptr);

	// One lenient typed read. A missing key, a non-object receiver, or a value
	// of the wrong type all yield a_default; nothing here throws or rejects.
	//
	// Integral T reads integers only. Floating T accepts any JSON number, so a
	// schema bound authored as `2` and as `2.0` both read the same — JSON erases
	// that distinction and authors use both.
	//
	// An unsigned T additionally refuses a negative rather than wrapping it:
	// nlohmann's is_number_integer() is true for signed values too, so a plain
	// get<unsigned>() on -1 silently yields 4294967295.
	template <class T>
		requires std::is_arithmetic_v<std::decay_t<T>> ||
		std::is_convertible_v<std::decay_t<T>, std::string_view>
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

	// The array/object at a_key, or nullptr when the key is missing or holds
	// anything else. Same lenient contract: a wrong type reads as absent.
	// Returned by pointer so the "present and of the right kind" test and the
	// use of the value are one step instead of a find()/is_*() pair.
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

	// Every string element of a_key's array, in order. Non-string elements are
	// skipped; a missing or non-array key yields an empty vector.
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

	// Typo/format-skew diagnostics: logs every key of a_obj not in a_known.
	// The caller picks the level via a_warn — true = WARN for runtime-owned files
	// (config.json and other OSF UI-owned documents, where an unknown key can only be a typo);
	// false = INFO for author-shipped files, where a newer mod on an older OSF UI release
	// makes unknown keys the normal compatible case (gate that call on devMode).
	// Never rejects; lenient parsing is the contract.
	void ReportUnknownKeys(const Value& a_obj, std::initializer_list<std::string_view> a_known, std::string_view a_sourceName, bool a_warn);

	// Logs one INFO line when a_obj's a_key is greater than a_known (the version
	// this build understands) — the "authored for a newer OSF UI" case, where
	// lenient parsing then ignores unknown fields. No-op otherwise. Absent key
	// defaults to a_known, so a file that omits the stamp stays silent.
	void CheckFormatVersion(const Value& a_obj, std::string_view a_key, std::int64_t a_known, std::string_view a_sourceName);
}
