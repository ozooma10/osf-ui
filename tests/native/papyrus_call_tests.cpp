#include "API/PapyrusCall.h"

#include <iostream>


namespace
{
	int failures = 0;

	void Check(const bool a_condition, const char* a_message)
	{
		if (!a_condition) {
			std::cerr << "FAIL: " << a_message << '\n';
			++failures;
		}
	}

	using OSFUI::PapyrusCall::Parse;
	using Arg = OSFUI::API::Papyrus::StaticCallArg;

	nlohmann::json Call(const char* a_script, const char* a_function, nlohmann::json a_args = nullptr)
	{
		nlohmann::json payload{ { "script", a_script }, { "function", a_function } };
		if (!a_args.is_null()) {
			payload["args"] = std::move(a_args);
		}
		return payload;
	}

	// Refused with this code?
	bool Refused(const nlohmann::json& a_payload, std::string_view a_code)
	{
		const auto parsed = Parse(a_payload);
		return !parsed.ok && parsed.code == a_code;
	}

	template <class T>
	bool ArgIs(const std::vector<Arg>& a_args, std::size_t a_index, const T& a_expected)
	{
		return a_index < a_args.size() &&
		       std::holds_alternative<T>(a_args[a_index]) &&
		       std::get<T>(a_args[a_index]) == a_expected;
	}
}

int main()
{
	Check(Refused(Call("OSFUI", "SetString"), "forbidden"),
		"OSFUI is refused as a papyrus.call target");
	Check(Refused(Call("osfui", "Reset"), "forbidden"),
		"the refusal is case-insensitive (Papyrus identifiers are)");
	Check(Refused(Call("OsFuI", "SetViewString"), "forbidden"),
		"mixed casing does not slip past the refusal");
	Check(Parse(Call("OSFUIHelper", "Go")).ok,
		"a script that merely starts with the platform name is fine");

	// --- name grammar --------------------------------------------------------
	Check(Parse(Call("MyMod:Sub:Script", "DoThing")).ok, "namespaced script names are accepted");
	Check(Refused(Call("", "DoThing"), "invalid-request"), "an empty script is refused");
	Check(Refused(Call("MyScript", ""), "invalid-request"), "an empty function is refused");
	Check(Refused(Call("9Script", "DoThing"), "invalid-request"), "identifiers cannot start with a digit");
	Check(Refused(Call("My Script", "DoThing"), "invalid-request"), "spaces are not identifiers");
	Check(Refused(Call("My:", "DoThing"), "invalid-request"), "a trailing namespace separator is refused");
	Check(Refused(Call("MyScript", "Do Thing"), "invalid-request"), "a function name is a bare identifier");
	Check(Refused(Call("MyScript", std::string(129, 'a').c_str()), "invalid-request"),
		"names are length-bounded");
	Check(Refused(nlohmann::json::object(), "invalid-request"), "a payload with no names is refused");

	// --- argument shape ------------------------------------------------------
	Check(Parse(Call("S", "F")).ok && Parse(Call("S", "F")).args.empty(),
		"omitting args means no args");
	Check(Refused(Call("S", "F", "nope"), "invalid-request"), "a non-array args is refused");
	Check(Refused(Call("S", "F", nlohmann::json::array({ nlohmann::json::object() })), "invalid-request"),
		"an arbitrary object is not a scalar");
	Check(Refused(Call("S", "F", nlohmann::json::array({ nlohmann::json::array() })), "invalid-request"),
		"a nested array is not a scalar");
	Check(Refused(Call("S", "F", nlohmann::json::array({ nullptr })), "invalid-request"),
		"null is not a scalar");
	{
		auto many = nlohmann::json::array();
		for (int i = 0; i < 32; ++i) many.push_back(i);
		Check(Parse(Call("S", "F", many)).ok, "32 arguments are allowed");
		many.push_back(33);
		Check(Refused(Call("S", "F", many), "invalid-request"), "33 arguments are refused");
	}

	// --- scalar types survive the boundary -----------------------------------
	{
		const auto parsed = Parse(Call("S", "F", nlohmann::json::array({ "text", 7, -7, true, false, 1.5 })));
		Check(parsed.ok && parsed.args.size() == 6, "a mixed scalar list marshals");
		Check(ArgIs<std::string>(parsed.args, 0, std::string("text")), "strings stay strings");
		Check(ArgIs<std::int32_t>(parsed.args, 1, 7), "positive integers stay ints");
		Check(ArgIs<std::int32_t>(parsed.args, 2, -7), "negative integers stay ints");
		Check(ArgIs<bool>(parsed.args, 3, true) && ArgIs<bool>(parsed.args, 4, false),
			"booleans stay booleans");
		Check(ArgIs<float>(parsed.args, 5, 1.5f), "fractional numbers become floats");
	}

	{
		const auto tagged = nlohmann::json{ { "$papyrus", "float" }, { "value", 3 } };
		const auto parsed = Parse(Call("S", "F", nlohmann::json::array({ tagged })));
		Check(parsed.ok && ArgIs<float>(parsed.args, 0, 3.0f),
			"osfui.papyrus.float(3) marshals as a float, not an int");
		Check(!Parse(Call("S", "F", nlohmann::json::array({ 3 }))).args.empty() &&
				std::holds_alternative<std::int32_t>(Parse(Call("S", "F", nlohmann::json::array({ 3 }))).args[0]),
			"a bare whole number is still an int");
		Check(Refused(Call("S", "F", nlohmann::json::array({
				  nlohmann::json{ { "$papyrus", "float" }, { "value", "x" } } })),
				  "invalid-request"),
			"a tagged float needs a numeric value");
		Check(Refused(Call("S", "F", nlohmann::json::array({
				  nlohmann::json{ { "$papyrus", "int" }, { "value", 3 } } })),
				  "invalid-request"),
			"no other tag is honored");
		Check(Refused(Call("S", "F", nlohmann::json::array({
				  nlohmann::json{ { "$papyrus", "float" }, { "value", 3 }, { "extra", 1 } } })),
				  "invalid-request"),
			"the tagged-float shape is exact");
		// The refusal reports the FLOAT reason, not the generic "not a scalar".
		const auto huge = nlohmann::json{ { "$papyrus", "float" }, { "value", 1e300 } };
		const auto parsed2 = Parse(Call("S", "F", nlohmann::json::array({ huge })));
		Check(!parsed2.ok && parsed2.message.find("float") != std::string::npos,
			"an out-of-range tagged float is refused as a float problem");
	}

	Check(Parse(Call("S", "F", nlohmann::json::array({ 2147483647 }))).ok, "INT32_MAX fits");
	Check(Parse(Call("S", "F", nlohmann::json::array({ -2147483648LL }))).ok, "INT32_MIN fits");
	Check(Refused(Call("S", "F", nlohmann::json::array({ 2147483648LL })), "invalid-request"),
		"INT32_MAX + 1 is refused");
	Check(Refused(Call("S", "F", nlohmann::json::array({ -2147483649LL })), "invalid-request"),
		"INT32_MIN - 1 is refused");
	Check(Refused(Call("S", "F", nlohmann::json::array({ 18446744073709551615ULL })), "invalid-request"),
		"a huge unsigned literal is refused rather than wrapping to a small negative");
	Check(Refused(Call("S", "F", nlohmann::json::array({ 9223372036854775808ULL })), "invalid-request"),
		"INT64_MAX + 1 is refused");
	Check(Parse(Call("S", "F", nlohmann::json::array({ 2147483647ULL }))).ok,
		"an unsigned value inside int32 range still marshals");
	{
		const auto parsed = Parse(Call("S", "F", nlohmann::json::array({ 4294967295ULL })));
		Check(!parsed.ok, "a u32 value above INT32_MAX is refused, not reinterpreted");
	}

	// --- float finiteness ----------------------------------------------------
	Check(Refused(Call("S", "F", nlohmann::json::array({ 1e300 })), "invalid-request"),
		"a double beyond float range is refused");
	Check(Parse(Call("S", "F", nlohmann::json::array({ 3.4e38 }))).ok, "a value inside float range is kept");

	if (failures == 0) {
		std::cout << "papyrus_call_tests: ok\n";
	}
	return failures;
}
