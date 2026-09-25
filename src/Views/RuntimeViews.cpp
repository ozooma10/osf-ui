#include "Runtime/Runtime.h"

#include <utility>
#include <vector>

#include "API/BridgeApi.h"
#include "Core/Ids.h"
#include "Core/Log.h"
#include "Core/Version.h"

namespace OSFUI
{
    bool Runtime::InstantiateView(const ViewManifest& a_manifest, std::string_view a_reason)
	{
		const auto& id = a_manifest.id;
		if (_presentation.IsInstantiated(id)) {
			return true;
		}
		if (!_renderer) {
			return false;
		}

		if (_developerMode) {
			_renderer->SetConsoleHandler(id, [id](int a_level, std::string a_message) {
				if (a_level == 2) {
					REX::ERROR("Runtime: view '{}' console: {}", id, a_message);
				} else if (a_level == 1) {
					REX::WARN("Runtime: view '{}' console: {}", id, a_message);
				} else {
					REX::DEBUG("Runtime: view '{}' console: {}", id, a_message);
				}
			});
		}

		m_viewRecovery.Clear(id);
		NavigateView(a_manifest);
		_presentation.AddInstantiated({ id, a_manifest.kind, a_manifest.capturesInput, a_manifest.pausesGame, a_manifest.order });
		_viewOpens.OnInstantiated(id);

		REX::INFO("Runtime: view '{}' instantiated {} ({}, capturesInput={}, pausesGame={})", id, a_reason, a_manifest.kind == ViewKind::Hud ? "hud" : "menu", a_manifest.capturesInput, a_manifest.pausesGame);
		if (_bridge) {
			API::BridgeApi::Get().SetBridgeAvailability(_bridge.get());
		}
		return true;
	}

	void Runtime::OnViewLoad(std::string_view a_viewId, bool a_failed, std::string_view a_url, std::string_view a_description, int a_errorCode)
	{
		const std::string id(a_viewId);
		// A navigation creates a fresh document; no pointer ownership crosses that boundary.
		_relativePointer.Cancel(id);
		if (!a_failed && _rendererFailed && _browserHostRecovery.CanAcceptResponse()) {
			const auto attempts = _browserHostRecovery.Attempts();
			_browserHostRecovery.OnResponse(_nowSeconds);
			API::BridgeApi::Get().SetBridgeAvailability(_bridge.get());
			_rendererFailed = false;
			_rendererFailureLatched = false;
			_osfSettings.ClearFailure("runtime.renderer");
			REX::INFO("Runtime: replacement browser host responded on attempt {}; menus remain closed; requested HUDs resume after loading", attempts);
		}
		m_viewLoads.FinishLoad(id, a_failed);
		_viewOpens.OnLoad(id, a_failed);
		if (!a_failed) {
			if (m_viewRecovery.Clear(id)) {
				REX::INFO("Runtime: view '{}' recovered ({})", a_viewId, a_url);
			} else {
				REX::INFO("Runtime: view '{}' finished loading ({})", a_viewId, a_url);
			}
			_osfSettings.ClearFailure("view.load-retrying:" + id);
			_osfSettings.ClearFailure("view.load-failed:" + id);
			BroadcastViewsData();  // loadState loading -> loaded
			return;
		}
		REX::ERROR("Runtime: view '{}' FAILED to load ({}): {} [{}]", a_viewId, a_url, a_description, a_errorCode);

		const auto recovery = m_viewRecovery.ScheduleFailure(id, _nowSeconds);
		if(recovery.exhausted) {
			REX::ERROR("view '{}' has exhausted its crash-recovery budget; destroying and unregistering the view (fix its files and relaunch)", a_viewId);
			_osfSettings.ClearFailure("view.load-retrying:" + id);
			_osfSettings.ReportFailure("view.load-failed:" + id, "view.load-failed", a_description, { { "view", id }, { "errorCode", a_errorCode } });
			TearDownFailedView(id);
			return;
		}

		REX::WARN("view '{}' load failed; crash-recovery will attempt reload in {:.0f} seconds (attempt {} of {})", a_viewId, recovery.retryDelay, recovery.nextAttempt, ViewRecoveryTracker::kMaxAttempts);
		_osfSettings.ClearFailure("view.load-failed:" + id);
		_osfSettings.ReportFailure("view.load-retrying:" + id, "view.load-retrying", a_description,
			{ { "view", id }, { "errorCode", a_errorCode }, { "attemptsLeft", recovery.attemptsRemaining } });
		BroadcastViewsData();  // loadState loading -> failed
	}

	void Runtime::NavigateView(const ViewManifest& a_manifest)
	{
		const auto& id = a_manifest.id;
		_relativePointer.Cancel(id);
		m_viewLoads.BeginLoad(id);
		m_viewInputGrants.ResetPage(id);
		_renderer->CreateOrNavigateView(a_manifest);
		if (_bridge) _bridge->OnViewCreated(id);
		API::BridgeApi::Get().SetViewInstantiated(id, true);
		// Geometry is renderer-wide, retained across restart and replayed in its
		// connection snapshot. Navigation does not need to resend unchanged sizes.
	}

	void Runtime::DriveRecovery()
	{
		if (_rendererFailed || !_renderer) {
			return;
		}

		for(const auto& id : m_viewRecovery.TakeDue(_nowSeconds)) {
			const auto* manifest = _views.Find(id);
			if(!manifest) {
				continue;
			}

			const auto attempt = m_viewRecovery.BeginAttempt(id);
			REX::INFO("Runtime: crash-recovery reloading view '{}' (attempt {} of {})", id, attempt, ViewRecoveryTracker::kMaxAttempts);
			NavigateView(*manifest);
		}
	}

	void Runtime::TearDownFailedView(const std::string& a_id)
	{
		m_viewRecovery.Clear(a_id);
		_viewOpens.Cancel(a_id);
		if (_renderer) {
			_renderer->DestroyView(a_id);
		}
		if (_presentation.RemoveInstantiated(a_id)) {
			ApplyViewPresentationPolicy();  // crash teardown may need to release input/pause now
		}
		API::BridgeApi::Get().SetViewInstantiated(a_id, false);
		bool instantiatedViewRemains = false;
		for (const auto& manifest : _views.All()) {
			if (_presentation.IsInstantiated(manifest.id)) {
				instantiatedViewRemains = true;
				break;
			}
		}
		if (!instantiatedViewRemains) {
			API::BridgeApi::Get().SetBridgeAvailability(nullptr);
		}
		if (_bridge) {
			_bridge->OnViewDestroyed(a_id);
		}
		m_viewInputGrants.ResetPage(a_id);
		BroadcastViewsData();
	}

	void Runtime::DriveDevTools()
	{
		if (!_devToolsRequested.exchange(false) || !_renderer) {
			return;
		}
		const auto active = _presentation.ActiveMenu();
		if (!active) {
			REX::DEBUG("Runtime: F12 DevTools — no open menu to inspect");
			return;
		}
		REX::INFO("Runtime: opening DevTools for view '{}'", *active);
		_renderer->OpenDevTools(*active);
	}

	void Runtime::PumpDevViewReload()
	{
		if (!_devViewReload) return;

		std::vector<DevViewReloadWorker::Target> targets;
		for (const auto& manifest : _views.All()) {
			if (_presentation.IsInstantiated(manifest.id)) {
				targets.push_back({ manifest.id });
			}
		}
		_devViewReload->SetTargets(std::move(targets));

		bool anyReloaded = false;
		for (const auto& completed : _devViewReload->DrainCompleted()) {
			const auto* manifest = _views.Find(completed.id);
			if (!manifest || !_presentation.IsInstantiated(completed.id)) continue;
			NavigateView(*manifest);
			anyReloaded = true;
			REX::INFO("Runtime: dev reloaded loose view '{}'", completed.id);
		}
		if (anyReloaded) BroadcastViewsData();
	}

	bool Runtime::HudAutoStartEligible(const ViewManifest& a_manifest) const
	{
		return a_manifest.kind == ViewKind::Hud && a_manifest.openOnStart &&
			(!a_manifest.debugOnly || _developerMode);
	}

	nlohmann::json Runtime::BuildViewsData() const
	{
		nlohmann::json views = nlohmann::json::array();
		const auto     active = _presentation.ActiveMenu();
		for (const auto& m : _views.All()) {
			const bool instantiated = _presentation.IsInstantiated(m.id);
			const auto state = m_viewLoads.GetState(m.id);
			const char* loadState =
				state == ViewLoadState::Failed   ? "failed" :
				state == ViewLoadState::Finished ? "loaded" :
				instantiated                     ? "loading" : "unloaded";
			views.push_back(nlohmann::json{
				{ "id", m.id },
				{ "title", m.title },
				{ "description", m.description },
				{ "mod", m.mod },
				{ "kind", m.kind == ViewKind::Hud ? "hud" : "menu" },
				{ "interactive", m.menuInputEligible },
				{ "open", _presentation.IsOpen(m.id) },
				{ "focused", active.has_value() && *active == m.id },
				{ "loadState", loadState },
				{ "openOnStart", m.openOnStart },
				{ "debugOnly", m.debugOnly },
			});
		}
		return nlohmann::json{ { "views", std::move(views) } };
	}
}
