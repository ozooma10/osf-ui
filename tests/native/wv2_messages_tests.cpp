
#include "Wv2Messages.h"

#include <cassert>
#include <iostream>
#include <string>

namespace msg = osfui::wv2::msg;
using nlohmann::json;

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

	// Serialize then parse back: what the peer actually receives.
	template <class T>
	[[nodiscard]] T RoundTrip(const T& a_msg)
	{
		return msg::FromJson<T>(msg::ToJson(a_msg));
	}
}

int main()
{
	// ---- every message stamps its own type, and the types are distinct.
	{
		Check(msg::ToJson(msg::Init{}).at("type") == "init", "init stamps type");
		Check(msg::ToJson(msg::Shutdown{}).at("type") == "shutdown", "fieldless message stamps type");
		Check(msg::ToJson(msg::Ready{}).at("type") == "ready", "ready stamps type");
		// The compatibility spelling is easy to "fix" by accident.
		Check(msg::SetInputTarget::kType == "setActive", "setActive wire spelling preserved");
		Check(msg::RelativePointerCapture::kType != msg::RelativePointer::kType,
			"relative-pointer state and motion use distinct directions");
		Check(msg::PointerInput::kType == "pointerInput",
			"pointer-transition state has a stable wire spelling");
		Check(msg::Viewport::kType == "viewport",
			"content viewport state has a stable wire spelling");
		Check(msg::Shutdown::kType != msg::DestroyView::kType, "distinct types");
	}

	// ---- round-trip fidelity across every field kind: u64, u32, i32, bool, string.
	{
		const msg::Init sent{
			.topLevelHwnd = 0x7FFF'FFFF'FFFF'FFFFull,
			.viewsPath = "C:/mods/views",
			.width = 2560,
			.height = 1440,
			.userDataDir = "C:/users/data",
			.devMode = true,
			.highRefreshCapture = true,
			.hidden = false,
			.adapterLuidLow = 4242,
			.adapterLuidHigh = 7,
		};
		const auto got = RoundTrip(sent);
		Check(got.topLevelHwnd == sent.topLevelHwnd, "u64 handle survives (above 2^53)");
		Check(got.viewsPath == sent.viewsPath, "string survives");
		Check(got.width == 2560 && got.height == 1440, "u32 survives");
		Check(got.devMode && got.highRefreshCapture && !got.hidden, "bools survive independently");
		Check(got.adapterLuidHigh == 7, "adapter luid survives");
	}
	{
		const msg::SetOrder sent{ .view = "acme.mod/panel", .order = -12 };
		const auto          got = RoundTrip(sent);
		Check(got.view == "acme.mod/panel" && got.order == -12, "negative i32 survives");
	}
	{
		const auto request = RoundTrip(msg::Focus{
			.focused = true, .epoch = 41, .view = "acme.mod/panel" });
		Check(request.focused && request.epoch == 41 && request.view == "acme.mod/panel",
			"focus request preserves desired state, epoch and target");
		const auto state = RoundTrip(msg::FocusState{
			.focused = true, .epoch = 41, .sequence = 9, .view = "acme.mod/panel" });
		Check(state.focused && state.epoch == 41 && state.sequence == 9 &&
			state.view == "acme.mod/panel",
			"focus acknowledgement preserves actual state ordering");
	}
	{
		const auto capture = RoundTrip(msg::RelativePointerCapture{
			.view = "acme.mod/panel", .active = true });
		Check(capture.view == "acme.mod/panel" && capture.active,
			"relative-pointer capture preserves its admitted view owner");
		const auto motion = RoundTrip(msg::RelativePointer{
			.view = "acme.mod/panel", .dx = -17, .dy = 23, .wheel = -120 });
		Check(motion.view == "acme.mod/panel" && motion.dx == -17 &&
			motion.dy == 23 && motion.wheel == -120,
			"signed raw relative-pointer motion survives host IPC");
	}
	{
		Check(!RoundTrip(msg::PointerInput{ .enabled = false }).enabled,
			"pointer input suspension survives host IPC");
		Check(msg::FromJson<msg::PointerInput>(json{ { "type", "pointerInput" } }).enabled,
			"a bare pointer-input message fails open for compatibility");
	}
	{
		const auto viewport = RoundTrip(msg::Viewport{
			.width = 2560, .height = 1440, .presentationEpoch = 17 });
		Check(viewport.width == 2560 && viewport.height == 1440 &&
			viewport.presentationEpoch == 17,
			"content viewport dimensions and presentation epoch survive host IPC");
	}
	{
		const msg::Frame sent{ .slot = 3, .serial = 0xDEAD'BEEF'0000'0001ull,
			.width = 1920, .height = 1080, .presentationEpoch = 9 };
		const auto got = RoundTrip(sent);
		Check(got.slot == 3 && got.serial == sent.serial && got.presentationEpoch == 9, "frame survives");
	}
	{
		const auto got = RoundTrip(msg::FrameAck{ .slot = 2, .serial = 77 });
		Check(got.slot == 2 && got.serial == 77, "frame acknowledgement preserves slot ownership");
	}
	{
		const msg::Textures sent{
			.width = 8,
			.height = 9,
			.slots = { 1ull, 2ull, 0xFFFF'FFFF'FFFFull },
			.produceFence = 111,
			.consumeFences = { 222, 333, 444 },
			.keyedMutex = true,
			.adapterLuidLow = 1,
			.adapterLuidHigh = 2,
		};
		const auto got = RoundTrip(sent);
		Check(got.slots == sent.slots, "slot handle array survives in order");
		Check(got.keyedMutex && got.produceFence == 111 &&
			got.consumeFences == sent.consumeFences, "per-slot fence handles survive in order");
	}
	{
		// Opaque already-serialized payloads must not be re-escaped or reparsed.
		const std::string inner = R"({"kind":"event","name":"x","payload":{"a":[1,2]}})";
		const auto        got = RoundTrip(msg::PostWeb{ .view = "acme.mod/v", .json = inner });
		Check(got.json == inner, "opaque bridge envelope survives byte-for-byte");
	}
	{
		// Non-ASCII must survive: Json::Dump is ensure_ascii=false.
		const auto got = RoundTrip(msg::Bye{ .reason = "終了 — done ✅" });
		Check(got.reason == "終了 — done ✅", "utf-8 payload survives");
	}

	{
		const auto got = msg::FromJson<msg::Navigate>(json{ { "type", "navigate" } });
		Check(got.entry == "index.html", "navigate.entry defaults to index.html");
		Check(got.logicalHeight == osfui::wv2::kDefaultLogicalHeight, "navigate.logicalHeight defaults to the shared constant");
		Check(got.id.empty(), "navigate.id defaults empty (the caller rejects it)");
	}
	{
		const auto got = msg::FromJson<msg::SetHidden>(json{ { "type", "setHidden" } });
		Check(got.hidden, "setHidden.hidden defaults true — a bare message hides");
	}
	{
		const auto got = msg::FromJson<msg::Init>(json{ { "type", "init" } });
		Check(got.width == 1 && got.height == 1, "init dimensions default to 1, never 0");
		Check(!got.highRefreshCapture, "init high-refresh capture defaults off");
		Check(got.hidden, "init.hidden defaults true");
	}
	{
		const auto got = msg::FromJson<msg::Fatal>(json{ { "type", "fatal" } });
		Check(got.stage == "renderer", "fatal.stage default");
		Check(got.description == "terminal renderer failure", "fatal.description default");
	}
	{
		const auto got = msg::FromJson<msg::Mouse>(json{ { "type", "mouse" } });
		Check(got.kind == "move", "mouse.kind defaults to move");
		Check(got.x == 0 && got.y == 0 && got.wheel == 0 && got.button == 0 && !got.down, "mouse numerics default to 0");
	}

	{
		const auto got = msg::FromJson<msg::Navigate>(json{
			{ "type", "navigate" },
			{ "id", "acme.mod/v" },
			{ "entry", 42 },          // number where a string belongs
			{ "logicalHeight", "tall" },
		});
		Check(got.id == "acme.mod/v", "good field still read alongside bad ones");
		Check(got.entry == "index.html", "wrong-typed string falls back to default");
		Check(got.logicalHeight == osfui::wv2::kDefaultLogicalHeight, "wrong-typed number falls back to default");
	}
	{
		Check(msg::FromJson<msg::Focus>(json::array({ 1, 2 })).focused == false, "array document reads as defaults");
		Check(msg::FromJson<msg::Bye>(json("nope")).reason.empty(), "string document reads as defaults");
		Check(msg::FromJson<msg::Cursor>(json()).id == 0, "null document reads as defaults");
	}
	{
		const auto got = msg::FromJson<msg::Cursor>(json{ { "type", "cursor" }, { "id", 5u }, { "shapeHint", "beam" }, { "future", { 1, 2 } } });
		Check(got.id == 5, "unknown fields ignored, known field still read");
	}
	{
		const auto got = msg::FromJson<msg::Resize>(json{ { "type", "resize" }, { "width", -1 }, { "height", 1080 } });
		Check(got.width == 1, "negative into unsigned keeps the default, no wrap");
		Check(got.height == 1080, "sibling field unaffected");
	}
	{
		// Slot array with a junk element: skipped, not defaulted to a null handle.
		const auto got = msg::FromJson<msg::Textures>(json{ { "type", "textures" }, { "slots", json::array({ 7u, "junk", 9u }) } });
		Check(got.slots.size() == 2 && got.slots[0] == 7 && got.slots[1] == 9, "non-integer slot entries are skipped");
		Check(msg::FromJson<msg::Textures>(json{ { "type", "textures" } }).slots.empty(), "absent slots array reads empty, not a throw");
		Check(msg::FromJson<msg::Textures>(json{ { "type", "textures" }, { "slots", "not-an-array" } }).slots.empty(), "wrong-typed slots reads empty");
		Check(msg::FromJson<msg::Textures>(json{ { "type", "textures" }, { "consumeFences", "not-an-array" } }).consumeFences.empty(), "wrong-typed consume fences read empty");
	}

	{
		const auto got = RoundTrip(msg::Hello{
			.protocolVersion = osfui::wv2::kBrowserHostProtocolVersion,
			.hostVersion = "2.0.0",
			.runtimeVersion = "120.0.0.0",
			.pid = 4242 });
		Check(got.protocolVersion == osfui::wv2::kBrowserHostProtocolVersion, "hello carries the protocol version");
		Check(got.pid == 4242, "hello carries the pid");
		// A garbage hello must land on 0/0 so the gate rejects rather than admits.
		const auto junk = msg::FromJson<msg::Hello>(json{ { "type", "hello" }, { "protocolVersion", "six" }, { "pid", nullptr } });
		Check(junk.protocolVersion == 0 && junk.pid == 0, "unreadable hello fields read as 0 so the identity gate fails closed");
	}

	std::cout << "wv2_messages_tests: " << checks << " checks, " << failures  << " failure(s)\n";
	return failures;
}
