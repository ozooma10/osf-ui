// Host-side tests for the localized keycap-label pipeline (input/KeyLabels):
// fixed short forms for non-printing keys (localizable via chrome.keys.*),
// layout glyphs for printable keys, the fallback chain, and layout fixtures
// modelled on US ANSI and German QWERTZ (Z/Y swap, umlaut OEM row, dead keys,
// the ISO <> key). Fakes stand in for the one platform source
// (Platform::MakeKeyLabelSource). Assert-style; exit code = failure count.

#include "input/KeyLabels.h"

#include <map>

namespace
{
	int g_failures = 0;
	int g_checks = 0;

#define CHECK(expr)                                                                     \
	do {                                                                                \
		++g_checks;                                                                     \
		if (!(expr)) {                                                                  \
			++g_failures;                                                               \
			std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr);        \
		}                                                                               \
	} while (0)

	using OSFUI::BuildKeyLabels;
	using OSFUI::KeyLabels;
	using OSFUI::KeyLabelSource;
	using OSFUI::ScanCode;

	std::string LabelOf(const KeyLabels& a_labels, std::string_view a_name)
	{
		for (const auto& [name, label] : a_labels.labels) {
			if (name == a_name) {
				return label;
			}
		}
		return "<absent>";
	}

	// US ANSI: letters/digits label as themselves, punctuation as the US
	// glyphs; the ISO extra key has no glyph on this layout.
	KeyLabelSource UsSource()
	{
		static const std::map<ScanCode, std::string> kGlyphs = {
			{ 0x29, "`" }, { 0x0C, "-" }, { 0x0D, "=" },
			{ 0x1A, "[" }, { 0x1B, "]" }, { 0x2B, "\\" },
			{ 0x27, ";" }, { 0x28, "'" },
			{ 0x33, "," }, { 0x34, "." }, { 0x35, "/" },
		};
		KeyLabelSource source;
		source.glyph = [](ScanCode a_scan) -> std::string {
			// Letters: name == glyph on US. Digits likewise.
			const auto name = OSFUI::KeyName(a_scan);
			if (name.size() == 1) {
				return name;
			}
			const auto it = kGlyphs.find(a_scan);
			return it != kGlyphs.end() ? it->second : std::string{};
		};
		source.layoutName = [](ScanCode) { return std::string{}; };
		source.layoutTag = [] { return std::string("en-US"); };
		return source;
	}

	// German QWERTZ: Z/Y swapped, umlauts on the US punctuation row, dead keys
	// ^ and ´, ß on the minus position, <> present next to LShift.
	KeyLabelSource GermanSource()
	{
		static const std::map<ScanCode, std::string> kGlyphs = {
			{ 0x15, "Z" },   // US Y position types z
			{ 0x2C, "Y" },   // US Z position types y
			{ 0x27, "Ö" }, { 0x28, "Ä" }, { 0x1A, "Ü" },
			{ 0x0C, "ß" },
			{ 0x29, "^" },   // dead key: spacing accent as the label
			{ 0x0D, "´" },   // dead key
			{ 0x1B, "+" }, { 0x2B, "#" },
			{ 0x33, "," }, { 0x34, "." }, { 0x35, "-" },
			{ 0x56, "<" },   // the ISO key US ANSI doesn't have
		};
		KeyLabelSource source;
		source.glyph = [](ScanCode a_scan) -> std::string {
			if (const auto it = kGlyphs.find(a_scan); it != kGlyphs.end()) {
				return it->second;
			}
			const auto name = OSFUI::KeyName(a_scan);
			return name.size() == 1 ? name : std::string{};
		};
		source.layoutName = [](ScanCode) { return std::string{}; };
		source.layoutTag = [] { return std::string("de-DE"); };
		return source;
	}

	std::string EnglishOnly(std::string_view, std::string_view a_english)
	{
		return std::string(a_english);
	}
}

// InputRouter.cpp (KeyName) references the Log seam; standard test stub.
namespace OSFUI::Log
{
	void WarnOnce(std::once_flag& a_flag, std::string_view a_message)
	{
		std::call_once(a_flag, [&] { REX::test::Log("WARN", std::string(a_message)); });
	}

	bool DevMode() { return false; }
	void SetDevMode(bool) {}
}

int main()
{
	// ---- US layout: labels match the historical board ----------------------
	{
		const auto labels = BuildKeyLabels(UsSource(), EnglishOnly);
		CHECK(labels.layout == "en-US");
		CHECK(LabelOf(labels, "W") == "W");
		CHECK(LabelOf(labels, "1") == "1");
		CHECK(LabelOf(labels, "Semicolon") == ";");
		CHECK(LabelOf(labels, "Grave") == "`");
		CHECK(LabelOf(labels, "F10") == "F10");
		CHECK(LabelOf(labels, "LShift") == "Shift");
		CHECK(LabelOf(labels, "Backspace") == "Bksp");
		CHECK(LabelOf(labels, "Up") == "↑");
		CHECK(LabelOf(labels, "Numpad5") == "Num 5");
		CHECK(LabelOf(labels, "NumpadEnter") == "Num Enter");
		// No glyph and no layout name: the key name itself, never empty.
		CHECK(LabelOf(labels, "IntlBackslash") == "IntlBackslash");
		// Every nameable key got a row with a non-empty label.
		CHECK(labels.labels.size() >= 100);
		for (const auto& [name, label] : labels.labels) {
			CHECK(!label.empty());
		}
		// Sided modifiers stay distinct entries (shared display text is fine).
		CHECK(LabelOf(labels, "RShift") == "Shift");
		CHECK(LabelOf(labels, "LCtrl") == "Ctrl");
		CHECK(LabelOf(labels, "RCtrl") == "Ctrl");
	}

	// ---- German QWERTZ: the reported-bug cases ------------------------------
	{
		const auto labels = BuildKeyLabels(GermanSource(), EnglishOnly);
		CHECK(labels.layout == "de-DE");
		// The Y/Z swap follows the layout.
		CHECK(LabelOf(labels, "Y") == "Z");
		CHECK(LabelOf(labels, "Z") == "Y");
		// The umlaut row: the physical US-semicolon key prints Ö.
		CHECK(LabelOf(labels, "Semicolon") == "Ö");
		CHECK(LabelOf(labels, "Apostrophe") == "Ä");
		CHECK(LabelOf(labels, "LBracket") == "Ü");
		CHECK(LabelOf(labels, "Minus") == "ß");
		// Dead keys label as their spacing accents.
		CHECK(LabelOf(labels, "Grave") == "^");
		CHECK(LabelOf(labels, "Equals") == "´");
		// The ISO <> key exists here and is labeled.
		CHECK(LabelOf(labels, "IntlBackslash") == "<");
		// Non-printing keys are unaffected by the layout.
		CHECK(LabelOf(labels, "LShift") == "Shift");
		CHECK(LabelOf(labels, "F5") == "F5");
	}

	// ---- chrome.keys.* localization for non-printing keys -------------------
	{
		const auto labels = BuildKeyLabels(GermanSource(),
			[](std::string_view a_address, std::string_view a_english) -> std::string {
				if (a_address == "chrome.keys.LShift") {
					return "Umsch";
				}
				return std::string(a_english);
			});
		CHECK(LabelOf(labels, "LShift") == "Umsch");
		CHECK(LabelOf(labels, "RShift") == "Shift");  // its own address, untranslated
		// Printable keys never consult the catalog — the layout glyph wins.
		CHECK(LabelOf(labels, "Semicolon") == "Ö");
	}

	// ---- Fallback chain: glyph -> layout name -> the key name ---------------
	{
		KeyLabelSource source;
		source.glyph = [](ScanCode a_scan) -> std::string {
			return a_scan == 0x11 ? std::string("W") : std::string{};
		};
		source.layoutName = [](ScanCode a_scan) -> std::string {
			return a_scan == 0x27 ? std::string("LAYOUTNAME") : std::string{};
		};
		source.layoutTag = [] { return std::string{}; };
		const auto labels = BuildKeyLabels(source, EnglishOnly);
		CHECK(labels.layout.empty());
		CHECK(LabelOf(labels, "W") == "W");                    // glyph
		CHECK(LabelOf(labels, "Semicolon") == "LAYOUTNAME");   // layout name
		CHECK(LabelOf(labels, "Q") == "Q");                    // key name
		// Null callables never crash and still label everything.
		const auto bare = BuildKeyLabels(KeyLabelSource{}, nullptr);
		CHECK(LabelOf(bare, "LShift") == "Shift");
		CHECK(LabelOf(bare, "Semicolon") == "Semicolon");
	}

	std::fprintf(stderr, "key_label_tests: %d checks, %d failure(s)\n", g_checks, g_failures);
	return g_failures;
}
