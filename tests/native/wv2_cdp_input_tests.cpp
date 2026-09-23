#include "Wv2CdpInput.h"
#include <cassert>
#include <iostream>

using namespace osfui::wv2;

int main()
{
	std::vector<std::string> sent;
	std::deque<CdpInputQueue::Completion> completions;
	int failures = 0;
	auto queue = std::make_shared<CdpInputQueue>(
		[&](const std::string& method, const nlohmann::json&, auto done) {
			sent.push_back(method);
			completions.push_back(std::move(done));
		}, [&] { ++failures; });
	auto complete = [&](bool ok) {
		auto callback = std::move(completions.front());
		completions.pop_front();
		callback(ok);
	};
	queue->Push("focus", {});
	queue->Push("down", {});
	queue->Push("char", {});
	queue->Push("up", {});
	assert(sent == std::vector<std::string>{ "focus" });
	complete(true);
	assert(sent.back() == "down" && sent.size() == 2);
	complete(true);
	assert(sent.back() == "char");
	queue->Close();
	// A replacement document can start its queue before the old CDP callback returns.
	queue = std::make_shared<CdpInputQueue>(
		[&](const std::string& method, const nlohmann::json&, auto done) {
			sent.push_back(method);
			completions.push_back(std::move(done));
		}, [&] { ++failures; });
	queue->Push("new-document-focus", {});
	complete(true);
	assert(sent.size() == 4 && sent.back() == "new-document-focus");
	assert(!queue->Idle()); // the old completion cannot advance the new document
	complete(true);
	assert(queue->Idle());
	assert(failures == 0);

	queue = std::make_shared<CdpInputQueue>(
		[&](const std::string&, const nlohmann::json&, auto done) { completions.push_back(std::move(done)); },
		[&] { ++failures; });
	queue->Push("down", {});
	queue->Push("up", {});
	complete(false);
	queue->Push("late", {});
	assert(completions.empty() && failures == 1);

	queue = std::make_shared<CdpInputQueue>(
		[&](const std::string&, const nlohmann::json&, auto done) { completions.push_back(std::move(done)); },
		[&] { ++failures; });
	for (int i = 0; i < 257; ++i) queue->Push("repeat", {});
	assert(failures == 2); // stalled CDP cannot grow memory without a limit
	complete(true);
	assert(completions.empty());
	queue = std::make_shared<CdpInputQueue>(
		[&](const std::string&, const nlohmann::json&, auto done) { completions.push_back(std::move(done)); },
		[&] { ++failures; });
	queue->Push("never-completes", {});
	queue->CheckTimeout(std::chrono::steady_clock::now() + std::chrono::seconds(6));
	assert(failures == 3 && queue->Idle());
	complete(true); // a late completion after timeout cannot restart the queue
	assert(completions.empty());

	CdpPressedKeys keys;
	msg::Keyboard left{ .vk = 0x10, .modifiers = 8, .location = 1, .down = true, .key = "Shift", .code = "ShiftLeft" };
	auto right = left;
	right.location = 2;
	right.code = "ShiftRight";
	keys.Observe(left);
	left.repeat = true;
	keys.Observe(left);
	keys.Observe(right);
	right.down = false;
	keys.Observe(right);
	const auto releases = keys.ReleaseAll();
	assert(releases.size() == 1 && !releases[0].down && !releases[0].repeat);
	assert(releases[0].location == 1 && releases[0].modifiers == 0);
	assert(keys.ReleaseAll().empty());
	const auto params = CdpKeyParams(left);
	assert(params["type"] == "rawKeyDown" && params["modifiers"] == 8 && params["autoRepeat"] == true);
	assert(!params.contains("text")); // WM_CHAR is the sole physical text stream
	const auto roundTrip = msg::FromJson<msg::Keyboard>(msg::ToJson(left));
	assert(roundTrip.code == left.code && roundTrip.repeat && roundTrip.location == 1);
	const auto text = msg::FromJson<msg::TextInput>(msg::ToJson(msg::TextInput{
		.text = "\xE6\x97\xA5", .kind = "composition", .cursor = 1 }));
	assert(text.text == "\xE6\x97\xA5" && text.cursor == 1 && text.kind == "composition");
	assert(!msg::FromJson<msg::WindowActive>(msg::ToJson(msg::WindowActive{ .active = false })).active);
	const auto mouse = msg::FromJson<msg::Mouse>(msg::ToJson(msg::Mouse{ .kind = "button", .down = true, .modifiers = 12 }));
	assert(mouse.down && mouse.modifiers == 12);

	Utf16Input utf;
	assert(utf.Push(u'A') == u"A");
	assert(utf.Push(0xD83D).empty());
	assert(utf.Push(0xDE00) == u"\U0001F600");
	assert(utf.Push(0xDE00).empty());
	assert(utf.Push(0xD83D).empty());
	utf.Reset();
	assert(utf.Push(0xDE00).empty()); // close/reopen cannot join unrelated input
	assert(utf.Push(0xD83D).empty());
	assert(utf.Push(u'B') == u"B");
	std::cout << "wv2_cdp_input_tests: passed\n";
}
