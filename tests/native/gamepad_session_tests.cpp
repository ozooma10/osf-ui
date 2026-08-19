#include "Input/GamepadSession.h"
#include "Input/ViewInputGrants.h"

#include <cassert>
#include <iostream>

int main()
{
	using OSFUI::GamepadSession;
	using Mode = GamepadSession::Mode;
	using OSFUI::XInputButton::kA;
	using OSFUI::XInputButton::kB;
	using OSFUI::XInputButton::kDPadDown;
	using OSFUI::XInputButton::kDPadLeft;
	using OSFUI::XInputButton::kDPadRight;
	using OSFUI::XInputButton::kDPadUp;
	using State = OSFUI::XInputPoller::State;

	{
		GamepadSession session;
		State state{ .connected = true, .buttons = kA };
		auto frame = session.Update(state, Mode::kDefault, 0.0, 0.0);
		assert(session.Active());
		assert(frame.buttonEdgeCount == 0);  // held-at-open is only a baseline

		state.buttons = kA | kB;
		frame = session.Update(state, Mode::kDefault, 0.0, 0.1);
		assert(frame.buttonEdgeCount == 1);
		assert(frame.buttonEdges[0].idCode == kB);
		assert(frame.buttonEdges[0].down);
		assert(frame.buttonEdges[0].action == GamepadSession::Action::kBack);

		state.buttons = 0;
		frame = session.Update(state, Mode::kDefault, 0.0, 0.2);
		assert(frame.buttonEdgeCount == 2);
		assert(frame.buttonEdges[0].idCode == kA && !frame.buttonEdges[0].down);
		assert(frame.buttonEdges[1].idCode == kB && !frame.buttonEdges[1].down);
		assert(frame.buttonEdges[0].action == GamepadSession::Action::kNone);
		assert(frame.buttonEdges[1].action == GamepadSession::Action::kNone);
	}

	{
		GamepadSession session;
		State state{ .connected = true };
		(void)session.Update(state, Mode::kDefault, 0.0, 0.0);
		state.buttons = kDPadUp | kDPadDown | kDPadLeft | kDPadRight | kA | kB;
		const auto frame = session.Update(state, Mode::kDefault, 0.0, 0.1);
		assert(frame.buttonEdgeCount == 6);
		assert(frame.buttonEdges[0].action == GamepadSession::Action::kUp);
		assert(frame.buttonEdges[1].action == GamepadSession::Action::kDown);
		assert(frame.buttonEdges[2].action == GamepadSession::Action::kLeft);
		assert(frame.buttonEdges[3].action == GamepadSession::Action::kRight);
		assert(frame.buttonEdges[4].action == GamepadSession::Action::kActivate);
		assert(frame.buttonEdges[5].action == GamepadSession::Action::kBack);
	}

	{
		GamepadSession session;
		State state{ .connected = true };
		(void)session.Update(state, Mode::kRaw, 0.0, 0.0);
		state.buttons = kDPadUp;
		auto frame = session.Update(state, Mode::kRaw, 0.0, 0.1);
		assert(frame.buttonEdgeCount == 1);
		assert(frame.buttonEdges[0].action == GamepadSession::Action::kNone);
	}

	{
		GamepadSession session;
		State state{ .connected = true, .lx = 0.60f };
		auto frame = session.Update(state, Mode::kDefault, 0.0, 0.0);
		assert(frame.axesChanged);
		assert(frame.navigationAction == GamepadSession::Action::kRight);

		frame = session.Update(state, Mode::kDefault, 0.0, 0.20);
		assert(!frame.axesChanged);
		assert(frame.navigationAction == GamepadSession::Action::kNone);

		frame = session.Update(state, Mode::kDefault, 0.0, 0.55);
		assert(frame.navigationAction == GamepadSession::Action::kRight);
	}

	{
		GamepadSession session;
		State state{ .connected = true, .ry = 0.50f };
		auto frame = session.Update(state, Mode::kRaw, 0.25, 0.0);
		assert(frame.axesChanged);
		assert(frame.navigationAction == GamepadSession::Action::kNone);
		assert(frame.wheelDelta == 0);

		frame = session.Update(state, Mode::kDefault, 0.25, 0.1);
		assert(!frame.axesChanged);
		assert(frame.wheelDelta == 120);
	}

	{
		GamepadSession session;
		State state{ .connected = true, .buttons = kA, .lx = 0.60f };
		auto frame = session.Update(state, Mode::kDefault, 0.0, 0.0);
		assert(frame.buttonEdgeCount == 0);
		assert(frame.axesChanged);
		assert(session.End());
		assert(!session.End());

		frame = session.Update(state, Mode::kDefault, 0.0, 1.0);
		assert(frame.buttonEdgeCount == 0);  // a new session baselines again
		assert(frame.axesChanged);           // axes dedupe does not leak across sessions
		assert(frame.navigationAction == GamepadSession::Action::kRight);
	}

	{
		GamepadSession session;
		State state{ .connected = true, .lx = 0.8f, .ry = 0.8f };
		(void)session.Update(state, Mode::kButtons, 0.25, 0.0);
		state.buttons = kDPadUp;
		auto frame = session.Update(state, Mode::kButtons, 0.25, 0.1);
		assert(frame.buttonEdges[0].action == GamepadSession::Action::kUp);
		assert(frame.navigationAction == GamepadSession::Action::kNone);  // first action came from the edge
		assert(frame.wheelDelta == 0);                                    // sticks are native-owned in this mode

		frame = session.Update(state, Mode::kButtons, 0.25, 0.66);
		assert(frame.navigationAction == GamepadSession::Action::kUp);  // repeat is session-owned
		assert(frame.wheelDelta == 0);
	}

	{
		OSFUI::ViewInputGrants grants;
		assert(grants.GamepadModeFor("view") == Mode::kDefault);
		grants.SetGamepadMode("view", Mode::kButtons);
		assert(grants.GamepadModeFor("view") == Mode::kButtons);
		grants.SetGamepadMode("view", Mode::kRaw);
		assert(grants.GamepadModeFor("view") == Mode::kRaw);
		assert(!grants.OwnsBackAction("view"));
		assert(!grants.BackTargetFor("view"));
		grants.SetBackOwnership("view", true, "osfui/settings");
		assert(grants.OwnsBackAction("view"));
		assert(grants.BackTargetFor("view") == std::optional<std::string>("osfui/settings"));
		grants.ResetPage("view");
		assert(grants.GamepadModeFor("view") == Mode::kDefault);
		assert(!grants.OwnsBackAction("view"));
		assert(!grants.BackTargetFor("view"));
	}

	std::cout << "gamepad session tests passed\n";
}
