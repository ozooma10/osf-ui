// Source-linked characterization of the mature runtime's cross-component view
// lifecycle. This deliberately does not execute Starfield, WebView2, or D3D12;
// it makes the proven ordering and fail-closed seams explicit so later edits
// cannot silently remove them without updating a focused test.

#include <fstream>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

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

	std::string ReadSource(const char* a_path)
	{
		std::ifstream input(a_path, std::ios::binary);
		if (!input) {
			std::cerr << "FAIL: could not read " << a_path << '\n';
			++failures;
			return {};
		}
		std::ostringstream contents;
		contents << input.rdbuf();
		return contents.str();
	}

	std::string FunctionBody(std::string_view a_source, std::string_view a_signature)
	{
		const auto signature = a_source.find(a_signature);
		if (signature == std::string_view::npos) {
			std::cerr << "FAIL: missing function signature: " << a_signature << '\n';
			++failures;
			return {};
		}
		const auto openingBrace = a_source.find('{', signature + a_signature.size());
		if (openingBrace == std::string_view::npos) {
			std::cerr << "FAIL: missing function body: " << a_signature << '\n';
			++failures;
			return {};
		}

		enum class LexState
		{
			Normal,
			LineComment,
			BlockComment,
			String,
			Character
		};

		LexState state = LexState::Normal;
		int depth = 0;
		for (std::size_t i = openingBrace; i < a_source.size(); ++i) {
			const char c = a_source[i];
			const char next = i + 1 < a_source.size() ? a_source[i + 1] : '\0';
			switch (state) {
			case LexState::LineComment:
				if (c == '\n') state = LexState::Normal;
				continue;
			case LexState::BlockComment:
				if (c == '*' && next == '/') {
					state = LexState::Normal;
					++i;
				}
				continue;
			case LexState::String:
				if (c == '\\') {
					++i;
				} else if (c == '"') {
					state = LexState::Normal;
				}
				continue;
			case LexState::Character:
				if (c == '\\') {
					++i;
				} else if (c == '\'') {
					state = LexState::Normal;
				}
				continue;
			case LexState::Normal:
				break;
			}

			if (c == '/' && next == '/') {
				state = LexState::LineComment;
				++i;
			} else if (c == '/' && next == '*') {
				state = LexState::BlockComment;
				++i;
			} else if (c == '"') {
				state = LexState::String;
			} else if (c == '\'') {
				state = LexState::Character;
			} else if (c == '{') {
				++depth;
			} else if (c == '}' && --depth == 0) {
				return std::string(a_source.substr(openingBrace, i - openingBrace + 1));
			}
		}

		std::cerr << "FAIL: unterminated function body: " << a_signature << '\n';
		++failures;
		return {};
	}

	bool ContainsInOrder(std::string_view a_text, std::initializer_list<std::string_view> a_needles)
	{
		std::size_t position = 0;
		for (const auto needle : a_needles) {
			position = a_text.find(needle, position);
			if (position == std::string_view::npos) {
				return false;
			}
			position += needle.size();
		}
		return true;
	}

	std::size_t Count(std::string_view a_text, std::string_view a_needle)
	{
		std::size_t count = 0;
		std::size_t position = 0;
		while ((position = a_text.find(a_needle, position)) != std::string_view::npos) {
			++count;
			position += a_needle.size();
		}
		return count;
	}
}

int main()
{
	const auto runtimeSource = ReadSource("../../src/Runtime/Runtime.cpp") +
		ReadSource("../../src/Runtime/RuntimeFrame.cpp");
	const auto rendererSource = ReadSource("../../src/Render/WebView2HostWebRenderer.cpp");
	const auto menuEventSource = ReadSource("../../src/Input/MenuEventSink.cpp");
	const auto presentationSource = ReadSource("../../src/Views/ViewPresentationController.cpp");
	const auto lifecycleHeader = ReadSource("../../src/Views/ViewLifecycle.h");

	const auto tick = FunctionBody(runtimeSource, "void Runtime::Tick(double a_deltaSeconds)");
	Check(ContainsInOrder(tick, {
		"TakePresentationRequests()",
		"PreparePresentationRequests(presentationWork)",
		"ProcessBackendQueues()",
		"ApplyPresentationRequests(presentationWork)" }),
		"Tick must instantiate requested views hidden, flush native sends, then apply visibility");
	const auto backendQueues = FunctionBody(runtimeSource, "void Runtime::ProcessBackendQueues()");
	Check(backendQueues.find("API::BridgeApi::Get().PumpMainThread()") != std::string::npos,
		"the extracted backend phase must still flush native sends before presentation is applied");

	const auto applyRequests = FunctionBody(runtimeSource,
		"void Runtime::ApplyPresentationRequests(const PendingPresentationWork& a_work)");
	Check(ContainsInOrder(applyRequests, {
		"case PresentationRequest::ToggleDefault:",
		"CancelPendingOpen()",
		"case PresentationRequest::Back:",
		"CancelPendingOpen()",
		"case PresentationRequest::CloseAll:",
		"CancelPendingOpen()",
		"_presentation.CloseAll()" }),
		"toggle, Escape/Back, and CloseAll must cancel a pending open");
	Check(ContainsInOrder(applyRequests, {
		"if (r.open)",
		"m_viewOpen.Target() == r.view",
		"CancelPendingOpen()",
		"_presentation.Close(r.view)" }),
		"a native close request must cancel its pending open before closing the view");

	const auto cancelPending = FunctionBody(runtimeSource, "bool Runtime::CancelPendingOpen()");
	Check(ContainsInOrder(cancelPending, {
		"_presentation.Close(kHandoffViewId)",
		"m_viewOpen.Cancel()" }),
		"pending-open cancellation must close the handoff surface before dropping ownership");

	const auto endpoints = FunctionBody(runtimeSource,
		"void Runtime::RegisterPlatformEndpoints(MessageBridge& a_bridge)");
	Check(ContainsInOrder(endpoints, {
		"RegisterSend(\"close\"",
		"CancelPendingOpen()",
		"_presentation.Close(a_b.CurrentSource())" }),
		"browser close must cancel a pending handoff or close the calling view");
	Check(ContainsInOrder(endpoints, {
		"const auto viewClose",
		"m_viewOpen.Targets(id)",
		"cancelled = CancelPendingOpen()",
		"_presentation.Close(id)",
		"RegisterRequest(\"menu.close\", viewClose)" }),
		"browser menu.close must cancel a pending open before applying the closed state");
	Check(ContainsInOrder(endpoints, {
		"const auto* manifest = _views.Find(id)",
		"if (!manifest)",
		"Reject(\"unknown-view\"",
		"!_captureIntegrationAvailable",
		"Reject(\"input-unavailable\"",
		"EnqueueOpenView(std::move(id))" }),
		"browser menu.open must fail closed before an unknown or unsafe menu is queued");

	const auto applyPolicy = FunctionBody(runtimeSource, "void Runtime::ApplyViewPresentationPolicy()");
	Check(ContainsInOrder(applyPolicy, {
		"DesiredCapture() && !_captureIntegrationAvailable",
		"CancelPendingOpen()",
		"_presentation.CloseActiveMenu()" }),
		"central presentation policy must fail closed when capture integration is unavailable");
	Check(ContainsInOrder(applyPolicy, {
		"visible && !wasVisible",
		"m_viewReveal.Arm()" }),
		"a cold or reopened presentation must arm the fresh-frame reveal gate");
	Check(ContainsInOrder(applyPolicy, {
		"if (!visible)",
		"m_viewReveal.Cancel()" }),
		"closing during a held reveal must cancel the reveal gate");

	const auto setHidden = FunctionBody(rendererSource,
		"void WebView2HostWebRenderer::SetViewHidden(std::string_view a_viewId, bool a_hidden)");
	Check(ContainsInOrder(setHidden, {
		"wasAllHidden",
		"RecomputeAllHidden()",
		"wasAllHidden && !_impl->allHidden",
		"++_impl->presentationEpoch",
		"_impl->haveFrame = false",
		".presentationEpoch = presentation" }),
		"closed-to-open must advance the host presentation epoch and invalidate the cached frame");

	const auto onFrame = FunctionBody(rendererSource, "void OnFrameMessage(const msg::Frame& a_msg)");
	Check(ContainsInOrder(onFrame, {
		"allHidden || presentation != presentationEpoch",
		"haveFrame = false",
		"ackNew = true" }),
		"all-hidden and wrong-epoch frames must be rejected and acknowledged");

	const auto submitFrame = FunctionBody(runtimeSource, "void Runtime::SubmitFrameIfVisible()");
	Check(ContainsInOrder(submitFrame, {
		"m_viewReveal.Observe(observation, _uptime)",
		"if (decision.submitFrame && frame)",
		"_compositor->Submit(*frame)",
		"if (decision.reveal)",
		"_compositor->SetVisible(true)" }),
		"Runtime must submit the gate-approved frame before making the compositor visible");
	Check(ContainsInOrder(submitFrame, {
		"if (!decision.timedOut)",
		"CancelPendingOpen()",
		"_presentation.CloseAll()",
		"ApplyViewPresentationPolicy()",
		"ReconcileFocusMenu()",
		"ReconcileControlLayer()",
		"ReconcileSimPause()",
		"FreeCursor::Apply(false)" }),
		"reveal timeout must close the presentation and immediately release every engine-owned input edge");

	const auto menuEvent = FunctionBody(menuEventSource, "MenuEventSink::ProcessEvent(");
	Check(ContainsInOrder(menuEvent, {
		"name == \"LoadingMenu\"",
		"name == \"MainMenu\"",
		"PresentationRequest::CloseAll" }),
		"LoadingMenu and MainMenu must enqueue the same CloseAll transition");

	const auto open = FunctionBody(presentationSource,
		"bool ViewPresentationController::Open(std::string_view a_id)");
	Check(open.find("_activeMenu = id") != std::string::npos &&
		open.find("_hudShown.clear()") == std::string::npos,
		"replacing the active menu must preserve open HUDs");

	const auto close = FunctionBody(presentationSource,
		"bool ViewPresentationController::Close(std::string_view a_id)");
	Check(close.find("_hudShown.erase(id)") != std::string::npos &&
		close.find("_activeMenu.reset()") != std::string::npos &&
		close.find("RemoveInstantiated") == std::string::npos &&
		close.find("DestroyView") == std::string::npos,
		"ordinary close must hide logical presentation state without destroying the reusable document");

	const auto closeAll = FunctionBody(presentationSource, "void ViewPresentationController::CloseAll()");
	Check(ContainsInOrder(closeAll, { "_activeMenu.reset()", "_hudShown.clear()" }),
		"CloseAll must close both the active menu and every HUD");

	const auto driveLifecycle = FunctionBody(runtimeSource, "void Runtime::DriveViewLifecycle()");
	Check(ContainsInOrder(driveLifecycle, {
		"actions.destroy",
		"_presentation.IsOpen(id)",
		"TearDownView(id, ViewTeardownReason::IdleReclaim)" }),
		"document destruction must remain behind the closed-view idle lifecycle");
	Check(lifecycleHeader.find("kSuspendAfterHiddenSeconds = 90.0") != std::string::npos &&
		lifecycleHeader.find("kDestroyAfterHiddenSeconds = 1500.0") != std::string::npos,
		"the mature reusable-document suspend and destruction thresholds must remain explicit");

	Check(Count(applyRequests, "CancelPendingOpen()") >= 4,
		"all four queued close paths must retain pending-open cancellation");

	if (failures == 0) {
		std::cout << "runtime_lifecycle_contract_tests: ok\n";
	}
	return failures;
}
