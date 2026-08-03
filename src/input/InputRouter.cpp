#include "input/InputRouter.h"

#include "core/Log.h"
#include "core/StringUtil.h"

namespace OSFUI
{
	namespace
	{
		struct NamedScan
		{
			std::string_view name;
			ScanCode         code;
		};

		// The one source of truth for both directions (ResolveKeyName / KeyName).
		// The FIRST spelling per code is canonical — KeyName returns it; the
		// remaining same-code rows are input aliases so hand-edited configs,
		// schema defaults, and W3C KeyboardEvent.code spellings resolve.
		//
		// Codes are set-1 make codes in the DirectInput convention (0x80 | base
		// for 0xE0-prefixed extended keys) — bit-identical to the engine
		// controlmap's DIK tokens. A name denotes a PHYSICAL position on the US
		// reference keyboard, so it means the same key on every machine and
		// every layout; the keycap a layout prints there is display data
		// (KeyLabels), never identity. Every name must stay ≤16 chars
		// (docs/authoring-views.md key-value constraint).
		constexpr NamedScan kNamedScans[] = {
			// Main block, digit row. Esc resolves but capture treats it as
			// cancel, so it is reserved rather than bindable.
			{ "Escape", 0x01 },
			{ "1", 0x02 }, { "2", 0x03 }, { "3", 0x04 }, { "4", 0x05 }, { "5", 0x06 },
			{ "6", 0x07 }, { "7", 0x08 }, { "8", 0x09 }, { "9", 0x0A }, { "0", 0x0B },
			{ "Minus", 0x0C }, { "Hyphen", 0x0C }, { "Dash", 0x0C },
			{ "Equals", 0x0D }, { "Equal", 0x0D }, { "Plus", 0x0D },
			{ "Backspace", 0x0E }, { "Tab", 0x0F },
			{ "Q", 0x10 }, { "W", 0x11 }, { "E", 0x12 }, { "R", 0x13 }, { "T", 0x14 },
			{ "Y", 0x15 }, { "U", 0x16 }, { "I", 0x17 }, { "O", 0x18 }, { "P", 0x19 },
			{ "LBracket", 0x1A }, { "LeftBracket", 0x1A }, { "BracketLeft", 0x1A },
			{ "RBracket", 0x1B }, { "RightBracket", 0x1B }, { "BracketRight", 0x1B },
			{ "Enter", 0x1C }, { "Return", 0x1C },
			{ "LCtrl", 0x1D }, { "ControlLeft", 0x1D },
			{ "A", 0x1E }, { "S", 0x1F }, { "D", 0x20 }, { "F", 0x21 }, { "G", 0x22 },
			{ "H", 0x23 }, { "J", 0x24 }, { "K", 0x25 }, { "L", 0x26 },
			{ "Semicolon", 0x27 },
			{ "Apostrophe", 0x28 }, { "Quote", 0x28 },
			// Console/grave key (top-left on US ANSI).
			{ "Grave", 0x29 }, { "Tilde", 0x29 }, { "Backtick", 0x29 },
			{ "Backquote", 0x29 }, { "Console", 0x29 },
			{ "LShift", 0x2A }, { "ShiftLeft", 0x2A },
			{ "Backslash", 0x2B },
			{ "Z", 0x2C }, { "X", 0x2D }, { "C", 0x2E }, { "V", 0x2F }, { "B", 0x30 },
			{ "N", 0x31 }, { "M", 0x32 },
			{ "Comma", 0x33 },
			{ "Period", 0x34 }, { "Dot", 0x34 },
			{ "Slash", 0x35 },
			{ "RShift", 0x36 }, { "ShiftRight", 0x36 },
			{ "NumpadMultiply", 0x37 },
			{ "LAlt", 0x38 }, { "AltLeft", 0x38 },
			{ "Space", 0x39 },
			{ "CapsLock", 0x3A },
			{ "F1", 0x3B }, { "F2", 0x3C }, { "F3", 0x3D }, { "F4", 0x3E },
			{ "F5", 0x3F }, { "F6", 0x40 }, { "F7", 0x41 }, { "F8", 0x42 },
			{ "F9", 0x43 }, { "F10", 0x44 },
			{ "NumLock", 0x45 }, { "ScrollLock", 0x46 },
			{ "Numpad7", 0x47 }, { "Numpad8", 0x48 }, { "Numpad9", 0x49 },
			{ "NumpadSubtract", 0x4A },
			{ "Numpad4", 0x4B }, { "Numpad5", 0x4C }, { "Numpad6", 0x4D },
			{ "NumpadAdd", 0x4E },
			{ "Numpad1", 0x4F }, { "Numpad2", 0x50 }, { "Numpad3", 0x51 },
			{ "Numpad0", 0x52 }, { "NumpadDecimal", 0x53 },
			// The extra ISO key between LShift and Z (<> on German boards):
			// previously unbindable because VK_OEM_102 had no name at all.
			{ "IntlBackslash", 0x56 }, { "Oem102", 0x56 },
			{ "F11", 0x57 }, { "F12", 0x58 },
			{ "F13", 0x64 }, { "F14", 0x65 }, { "F15", 0x66 }, { "F16", 0x67 },
			{ "F17", 0x68 }, { "F18", 0x69 }, { "F19", 0x6A }, { "F20", 0x6B },
			{ "F21", 0x6C }, { "F22", 0x6D }, { "F23", 0x6E },
			{ "IntlRo", 0x73 },
			{ "F24", 0x76 },
			{ "IntlYen", 0x7D },
			// Extended keys (0x80 | base).
			{ "NumpadEnter", 0x9C },
			{ "RCtrl", 0x9D }, { "ControlRight", 0x9D },
			{ "NumpadDivide", 0xB5 },
			// Delivered as key-up only; capture latches its release.
			{ "PrintScreen", 0xB7 }, { "PrtScn", 0xB7 },
			{ "RAlt", 0xB8 }, { "AltRight", 0xB8 },
			{ "Pause", 0xC5 },
			{ "Home", 0xC7 },
			{ "Up", 0xC8 }, { "ArrowUp", 0xC8 },
			{ "PageUp", 0xC9 },
			{ "Left", 0xCB }, { "ArrowLeft", 0xCB },
			{ "Right", 0xCD }, { "ArrowRight", 0xCD },
			{ "End", 0xCF },
			{ "Down", 0xD0 }, { "ArrowDown", 0xD0 },
			{ "PageDown", 0xD1 },
			{ "Insert", 0xD2 }, { "Delete", 0xD3 },
			// Nameable so hand-edited configs work, but capture-reserved: a Win
			// keyup outside exclusive fullscreen opens the Start menu.
			{ "LWin", 0xDB }, { "MetaLeft", 0xDB },
			{ "RWin", 0xDC }, { "MetaRight", 0xDC },
			{ "Apps", 0xDD }, { "ContextMenu", 0xDD },
		};
	}

	ScanCode ResolveKeyName(std::string_view a_name)
	{
		if (a_name.empty()) {
			return kInvalidScanCode;
		}

		// Scan space is not contiguous for any name family (F11/F12 sit apart
		// from F1-F10, letters follow the QWERTY rows), so everything —
		// including letters, digits, and F-keys — is explicit table rows.
		for (const auto& key : kNamedScans) {
			if (StringUtil::EqualsCaseInsensitiveAscii(key.name, a_name)) {
				return key.code;
			}
		}

		// Callers vary (toggle key, hotkey bindings, conflict grouping): an
		// unresolvable name simply does not bind.
		REX::WARN("InputRouter: could not resolve key name '{}'", a_name);
		return kInvalidScanCode;
	}

	std::string KeyName(ScanCode a_scan)
	{
		// Canonical (first) name per code; aliases like Return/Tilde resolve
		// back to Enter/Grave. The first kNamedScans row per code is the
		// canonical spelling, so the two directions cannot drift.
		for (const auto& key : kNamedScans) {
			if (key.code == a_scan) {
				return std::string(key.name);
			}
		}
		return {};
	}

	namespace
	{
		constexpr ScanCode kScanEscape = 0x01;
	}

	void InputRouter::Configure(ScanCode a_toggleKey, std::function<void()> a_onToggle,
		std::function<void()> a_onBack)
	{
		_toggleKey.store(a_toggleKey, std::memory_order_release);
		_onToggle = std::move(a_onToggle);
		_onBack = std::move(a_onBack);
	}

	void InputRouter::SetToggleKey(ScanCode a_toggleKey)
	{
		_toggleKey.store(a_toggleKey, std::memory_order_release);
	}

	void InputRouter::SetWebRouting(std::function<bool()> a_isCaptured,
		std::function<void(KeyCode, bool)> a_routeKey)
	{
		_isCaptured = std::move(a_isCaptured);
		_routeKey = std::move(a_routeKey);
	}

	void InputRouter::OnKeyDown(KeyCode a_vk, ScanCode a_scan)
	{
		// Fed by the WndProc hook. Toggle is handled before capture so it works
		// even while the overlay owns input, and is a distinct intent from a
		// captured Esc (Esc = back: close the top menu, or delegate to a
		// back-owning view via osfui.handleBack). Both match on the physical
		// scan code and are consumed here so the key never also routes into
		// the view as a plain keystroke.
		const bool captured = Captured();
		const auto toggleKey = _toggleKey.load(std::memory_order_acquire);
		if (toggleKey != kInvalidScanCode && a_scan == toggleKey) {
			if (_onToggle) {
				_onToggle();
			}
			return;
		}
		if (captured && a_scan == kScanEscape) {
			if (_onBack) {
				_onBack();
			}
			return;
		}

		if (captured && _routeKey) {
			_routeKey(a_vk, true);
			return;
		}
		if (Log::DevMode()) {
			REX::DEBUG("InputRouter: OnKeyDown(vk {}, scan {}) (overlay not capturing — passed to game)", a_vk, a_scan);
		}
	}

	void InputRouter::OnKeyUp(KeyCode a_vk, ScanCode /*a_scan*/)
	{
		if (Captured() && _routeKey) {
			_routeKey(a_vk, false);
			return;
		}
		if (Log::DevMode()) {
			REX::DEBUG("InputRouter: OnKeyUp({})", a_vk);
		}
	}
}
