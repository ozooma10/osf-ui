#pragma once

#include <cmath>
#include <limits>

#include <nlohmann/json.hpp>

#include "api/PapyrusApi.h"
#include "core/StringUtil.h"
#include "runtime/PapyrusNames.h"

// Validation and JS->Papyrus marshalling for the `papyrus.call` endpoint.
//
// Split out of the endpoint lambda so it is reachable from the host test
// suite: this is the one surface that lets untrusted view content name an
// arbitrary script and GLOBAL function, and every guard on it (the platform
// script refusal, the argument cap, the int range, the float finiteness) is
// only as good as the test that pins it. The endpoint in Runtime keeps the
// transport half — reading the source view and surfacing the failure.
namespace OSFUI::PapyrusCall
{
	// Papyrus itself has no hard parameter limit; this is a bound on untrusted
	// input, sized well past any plausible signature.
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

		[[nodiscard]] inline std::string StringField(const nlohmann::json& a_obj, std::string_view a_key)
		{
			const auto it = a_obj.find(a_key);
			return it != a_obj.end() && it->is_string() ? it->get<std::string>() : std::string{};
		}

		// Papyrus floats are 32-bit. A value JS can hold but the VM cannot is a
		// refusal, not a silent inf.
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
		out.script = detail::StringField(a_payload, "script");
		out.function = detail::StringField(a_payload, "function");
		if (!PapyrusNames::IsScriptName(out.script) || !PapyrusNames::IsIdentifier(out.function)) {
			return detail::Fail("invalid-request", "papyrus.call requires valid 'script' and 'function' names");
		}
		// OSF UI's own natives are not a callable mod surface. They take the
		// target mod id as an ARGUMENT and trust their caller (Papyrus is a
		// mod's own code), so reaching them from a page would hand it a trusted
		// alias for settings.set / settings.reset / state publishing WITHOUT the
		// Ids::ResolveWritableMod authority check those endpoints enforce —
		// rebinding OSF UI's own toggleKey, resetting a neighbour's settings, or
		// publishing state under another mod's identity. Case-insensitive:
		// Papyrus identifiers are, and BSFixedString interning does not preserve
		// the caller's spelling.
		if (StringUtil::EqualsCaseInsensitiveAscii(out.script, API::Papyrus::kPlatformScriptName)) {
			return detail::Fail("forbidden",
				"papyrus.call cannot target OSF UI's own script — use the osfui.* endpoints");
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
				// BEFORE is_number_integer(), which is true for unsigned too:
				// reading a > INT64_MAX literal as int64 is a modular cast, so
				// the refusal below would never fire and the value would reach
				// Papyrus as a small negative int.
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
				detail::StringField(value, "$papyrus") == "float" && value.contains("value") &&
				value["value"].is_number()) {
				// JSON erases the difference between 3 and 3.0. The helper's
				// tagged float keeps whole-valued Papyrus float parameters
				// expressible.
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
