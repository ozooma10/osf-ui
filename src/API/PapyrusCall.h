#pragma once

#include <cmath>
#include <limits>

#include "API/PapyrusApi.h"
#include "Core/StringUtil.h"
#include "Core/Json.h"
#include "API/PapyrusNames.h"

// Validates untrusted papyrus.call payloads before JS-to-Papyrus marshalling.
namespace OSFUI::PapyrusCall
{
	// Bound untrusted argument arrays even though Papyrus has no fixed limit.
	inline constexpr std::size_t kMaxArgs = 32;

	struct Parsed
	{
		bool                                     ok{ false };
		std::string                              code;     // set when !ok
		std::string                              message;  // set when !ok
		std::string                              script;
		std::string                              function;
		std::vector<API::Papyrus::StaticCallArg> args;
	};

	namespace detail
	{
		[[nodiscard]] inline Parsed Fail(std::string a_code, std::string a_message)
		{
			return Parsed{ .ok = false, .code = std::move(a_code), .message = std::move(a_message) };
		}

		// Reject values outside the finite 32-bit Papyrus float range.
		[[nodiscard]] inline bool AppendFloat(std::vector<API::Papyrus::StaticCallArg>& a_out, double a_number)
		{
			if (!std::isfinite(a_number) || std::abs(a_number) > std::numeric_limits<float>::max()) {
				return false;
			}
			a_out.emplace_back(static_cast<float>(a_number));
			return true;
		}
	}

	// Validate one `papyrus.call` payload and marshal its arguments.
	[[nodiscard]] inline Parsed Parse(const nlohmann::json& a_payload)
	{
		Parsed out;
		out.script = Json::Get(a_payload, "script", "");
		out.function = Json::Get(a_payload, "function", "");
		if (!PapyrusNames::IsScriptName(out.script) || !PapyrusNames::IsIdentifier(out.function)) {
			return detail::Fail("invalid-request", "papyrus.call requires valid 'script' and 'function' names");
		}
		// Reject every OSF UI native script so untrusted pages cannot bypass per-view authority.
		if (StringUtil::EqualsCaseInsensitiveAscii(out.script, API::Papyrus::kPlatformScriptName) ||
			StringUtil::EqualsCaseInsensitiveAscii(out.script, API::Papyrus::kSettingsScriptName) ||
			StringUtil::EqualsCaseInsensitiveAscii(out.script, API::Papyrus::kViewScriptName)) {
			return detail::Fail("forbidden", "papyrus.call cannot target OSF UI's own scripts — use the public bridge APIs");
		}

		const auto it = a_payload.find("args");
		if (it != a_payload.end() && !it->is_array()) {
			return detail::Fail("invalid-request", "papyrus.call 'args' must be an array");
		}
		const auto  empty = nlohmann::json::array();
		const auto& input = it == a_payload.end() ? empty : *it;
		if (input.size() > kMaxArgs) {
			return detail::Fail("invalid-request", "papyrus.call accepts at most 32 arguments");
		}

		out.args.reserve(input.size());
		for (const auto& value : input) {
			if (value.is_string()) {
				out.args.emplace_back(value.get<std::string>());
			} else if (value.is_boolean()) {
				out.args.emplace_back(value.get<bool>());
			} else if (value.is_number_unsigned()) {
				// Check unsigned first because nlohmann integer conversion can wrap large values.
				const auto integer = value.get<std::uint64_t>();
				if (integer > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
					return detail::Fail("invalid-request", "papyrus.call integer arguments must fit Papyrus int");
				}
				out.args.emplace_back(static_cast<std::int32_t>(integer));
			} else if (value.is_number_integer()) {
				const auto integer = value.get<std::int64_t>();
				if (integer < std::numeric_limits<std::int32_t>::min() ||
					integer > std::numeric_limits<std::int32_t>::max()) {
					return detail::Fail("invalid-request", "papyrus.call integer arguments must fit Papyrus int");
				}
				out.args.emplace_back(static_cast<std::int32_t>(integer));
			} else if (value.is_number_float()) {
				if (!detail::AppendFloat(out.args, value.get<double>())) {
					return detail::Fail("invalid-request", "papyrus.call float arguments must be finite Papyrus floats");
				}
			} else if (value.is_object() && value.size() == 2 &&
				Json::Get(value, "$papyrus", "") == "float" && value.contains("value") &&
				value["value"].is_number()) {
				// Tagged floats preserve whole-valued Papyrus float arguments.
				if (!detail::AppendFloat(out.args, value["value"].get<double>())) {
					return detail::Fail("invalid-request", "papyrus.call float arguments must be finite Papyrus floats");
				}
			} else {
				return detail::Fail("invalid-request",
					"papyrus.call arguments must be scalar values or osfui.papyrus.float(value)");
			}
		}
		out.ok = true;
		return out;
	}
}
