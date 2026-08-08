// The lenient accessor contract (src/runtime/Json.h).
//
// Json::Get is now the single reader for every JSON field in the plugin and the
// browser host — ~90 former Json::Get{String,Bool,Int} call sites plus ~90
// former nlohmann .value() sites. That makes its edge behaviour load-bearing in
// a way neither predecessor's was, and the host/renderer half had no test
// coverage at all. Each case below pins a promise Json.h makes:
//
//   - a missing key, a wrong-typed value, and a non-object receiver all yield
//     the caller's default, and none of them throws (.value() threw
//     type_error.302 / .306, on threads where an escape is a std::terminate);
//   - a negative never wraps into an unsigned target (.value("w", 1u) on -1
//     silently produced 4294967295);
//   - integral and floating targets differ on purpose: JSON erases 2 vs 2.0, so
//     schema bounds must accept both, while an int field must not silently
//     truncate a float.

#include "runtime/Json.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

namespace Json = OSFUI::Json;
using Json::Value;

namespace
{
	int checks = 0;
	int failures = 0;

	void Check(bool a_ok, std::string_view a_what)
	{
		++checks;
		if (!a_ok) {
			++failures;
			std::cout << "  FAIL: " << a_what << "\n";
		}
	}
}

int main()
{
	const Value obj{
		{ "s", "text" },
		{ "b", true },
		{ "i", 42 },
		{ "neg", -7 },
		{ "big", 9007199254740993ull },  // 2^53 + 1: lossy through a double
		{ "f", 2.5 },
		{ "whole", 2 },
		{ "arr", Value::array({ 1, 2 }) },
		{ "obj", Value::object({ { "k", "v" } }) },
		{ "null", nullptr },
	};

	// ---- reads of the right type
	Check(Json::Get(obj, "s", "fallback") == "text", "string read");
	Check(Json::Get(obj, "b", false) == true, "bool read");
	Check(Json::Get(obj, "i", 0) == 42, "int read");
	Check(Json::Get(obj, "neg", 0) == -7, "negative int read");
	Check(Json::Get(obj, "f", 0.0) == 2.5, "double read");
	Check(Json::Get(obj, "big", std::uint64_t{ 0 }) == 9007199254740993ull,
		"u64 above 2^53 survives exactly");

	// ---- a missing key yields the caller's default, whatever the type
	Check(Json::Get(obj, "nope", "fallback") == "fallback", "missing string");
	Check(Json::Get(obj, "nope", true) == true, "missing bool keeps true");
	Check(Json::Get(obj, "nope", 99) == 99, "missing int");
	Check(Json::Get(obj, "nope", 1.5) == 1.5, "missing double");

	// ---- a WRONG-TYPED value yields the default too. This is the half
	// .value() did not do: it threw.
	Check(Json::Get(obj, "i", "fallback") == "fallback", "number where string expected");
	Check(Json::Get(obj, "s", 99) == 99, "string where int expected");
	Check(Json::Get(obj, "s", false) == false, "string where bool expected");
	Check(Json::Get(obj, "i", false) == false, "number where bool expected");
	Check(Json::Get(obj, "b", 99) == 99, "bool where int expected");
	Check(Json::Get(obj, "arr", "fallback") == "fallback", "array where string expected");
	Check(Json::Get(obj, "obj", 99) == 99, "object where int expected");
	Check(Json::Get(obj, "null", "fallback") == "fallback", "null reads as absent");

	// ---- integral vs floating targets are deliberately different.
	// A float into an int field must NOT silently truncate...
	Check(Json::Get(obj, "f", 0) == 0, "2.5 into an int target keeps the default");
	// ...but an integer into a float field must be accepted, because JSON
	// erases the difference and schema bounds are authored both ways.
	Check(Json::Get(obj, "whole", 0.0) == 2.0, "integer into a double target is accepted");
	Check(Json::Get(obj, "f", 0.0) == 2.5, "double into a double target");

	// ---- a negative must never wrap into an unsigned target.
	Check(Json::Get(obj, "neg", std::uint32_t{ 1 }) == 1u,
		"negative into u32 keeps the default, no modular wrap");
	Check(Json::Get(obj, "neg", std::uint64_t{ 7 }) == 7ull, "negative into u64 keeps the default");
	Check(Json::Get(obj, "i", std::uint32_t{ 1 }) == 42u, "positive into u32 still reads");
	// The signed path is unaffected.
	Check(Json::Get(obj, "neg", std::int32_t{ 0 }) == -7, "negative into i32 reads");

	// ---- a non-object receiver reads as all-defaults instead of throwing.
	for (const Value& notAnObject : { Value(Value::array({ 1 })), Value("str"), Value(7), Value() }) {
		Check(Json::Get(notAnObject, "k", "fallback") == "fallback", "non-object receiver, string");
		Check(Json::Get(notAnObject, "k", 5) == 5, "non-object receiver, int");
		Check(Json::Get(notAnObject, "k", true), "non-object receiver, bool");
	}

	// ---- const char* defaults deduce to std::string, so `Get(o, "k", "")`
	// works exactly as `.value("k", "")` used to.
	{
		const auto s = Json::Get(obj, "s", "");
		static_assert(std::is_same_v<decltype(s), const std::string>,
			"a string-literal default must deduce to std::string");
		Check(s == "text", "literal default deduces to std::string");
		const std::string fallback = "owned";
		Check(Json::Get(obj, "nope", fallback) == "owned", "std::string default");
	}

	// ---- GetArray / GetObject: present-and-right-kind in one step, null
	// otherwise. Same lenient rule — a wrong type reads as absent.
	Check(Json::GetArray(obj, "arr") != nullptr && Json::GetArray(obj, "arr")->size() == 2,
		"GetArray returns the array");
	Check(Json::GetArray(obj, "obj") == nullptr, "GetArray on an object is null");
	Check(Json::GetArray(obj, "nope") == nullptr, "GetArray on a missing key is null");
	Check(Json::GetObject(obj, "obj") != nullptr, "GetObject returns the object");
	Check(Json::GetObject(obj, "arr") == nullptr, "GetObject on an array is null");
	Check(Json::GetArray(Value("not-an-object"), "arr") == nullptr,
		"GetArray on a non-object receiver is null");

	// ---- GetStringArray skips non-strings rather than failing the whole read.
	{
		const Value mixed{ { "a", Value::array({ "x", 1, "y", nullptr }) } };
		const auto  got = Json::GetStringArray(mixed, "a");
		Check(got.size() == 2 && got[0] == "x" && got[1] == "y", "non-string elements skipped");
		Check(Json::GetStringArray(obj, "s").empty(), "non-array reads empty");
	}

	// ---- Parse: comments allowed (the drop-in schema contract), malformed
	// input is std::nullopt rather than an exception.
	Check(Json::Parse(R"({"a":1})").has_value(), "plain object parses");
	Check(Json::Parse("{\n// a comment\n\"a\":1}").has_value(), "line comments are accepted");
	Check(Json::Parse("{/* block */\"a\":1}").has_value(), "block comments are accepted");
	Check(!Json::Parse("{ nope }").has_value(), "malformed input is nullopt, not a throw");
	Check(!Json::Parse("").has_value(), "empty input is nullopt");

	// ---- Dump: malformed UTF-8 is replaced, never thrown. A strict dump()
	// throws type_error.316 here, and most dump sites have no handler above them.
	{
		Value broken;
		broken["k"] = std::string("ok\xC3");  // truncated 2-byte sequence
		const auto text = Json::Dump(broken);
		Check(!text.empty() && text.find("ok") != std::string::npos,
			"malformed utf-8 dumps with replacement instead of throwing");
		Check(Json::Dump(Value{ { "u", "héllo ✅" } }).find("héllo") != std::string::npos,
			"valid non-ascii is not escaped (ensure_ascii=false)");
	}

	std::cout << "json_tests: " << checks << " checks, " << failures << " failure(s)\n";
	return failures;
}
