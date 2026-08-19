#include "Runtime/Runtime.h"

#include <format>
#include <utility>
#include <vector>

#include "API/PapyrusApi.h"
#include "API/PapyrusCall.h"
#include "Core/Ids.h"
#include "Core/Json.h"
#include "Views/BuiltinViewIds.h"

namespace OSFUI
{
    void Runtime::BroadcastViewsData()
	{
		if (!_bridge) {
			return;
		}
		auto dumped = Json::Dump(BuildViewsData());
		if (dumped == _lastViewsData) {
			return;
		}
		_lastViewsData = std::move(dumped);
		PublishPlatformState("views");
	}

	std::unordered_set<std::string> Runtime::InstantiatedViewsOfMod(std::string_view a_mod) const
	{
		std::unordered_set<std::string> targets;
		for (const auto& manifest : _views.All()) {
			if (!_presentation.IsInstantiated(manifest.id)) {
				continue;
			}
			if (Ids::EqualsCaseInsensitiveAscii(Ids::ModOf(manifest.id), a_mod)) {
				targets.insert(manifest.id);
			}
		}
		return targets;
	}

	void Runtime::PublishModState(std::string_view a_mod, std::string_view a_key, const nlohmann::json& a_value)
	{
		if (!_bridge) {
			return;
		}
		const auto targets = InstantiatedViewsOfMod(a_mod);
		if (targets.empty()) {
			REX::DEBUG("Runtime: state '{}/{}' has no instantiated view yet - retained for the next greeting", a_mod, a_key);
			return;
		}
		_bridge->PublishState(targets, a_mod, a_key, a_value);
	}

	void Runtime::PublishPlatformState(std::string_view a_key, std::string_view a_viewId)
	{
		if (!_bridge) {
			return;
		}
		const auto deliver = [&](const std::string& a_view) {
			if (a_key == "views") {
				if (_lastViewsData.empty()) {
					_lastViewsData = Json::Dump(BuildViewsData());
				}
				_bridge->PublishJsonState(a_view, "osfui", "views", _lastViewsData);
			} else if (a_key == "settings") {
				if (_settings) {
					_bridge->PublishState(a_view, "osfui", "settings", _settings->Store().DataView());
				}
			} else if (a_key == "diagnostics") {
				_bridge->PublishState(a_view, "osfui", "diagnostics", _healthRegistry.Snapshot());
			} else if (a_key == "keybindings") {
				_bridge->PublishState(a_view, "osfui", "keybindings", _controlMap.KeybindingsState());
			} else if (a_key == "input-context") {
				_bridge->PublishState(a_view, "osfui", "input-context", _controlMap.EngineInputContextState());
			} else if (a_key == "i18n") {
				const std::string mod{ Ids::ModOf(a_view) };
				_bridge->PublishState(a_view, "osfui", "i18n", nlohmann::json{
					{ "mod", mod },
					{ "locale", _localization.Locale() },
					{ "strings", _localization.CatalogFor(mod) },
				});
			}
		};
		if (!a_viewId.empty()) {
			deliver(std::string(a_viewId));
			return;
		}
		for (const auto& manifest : _views.All()) {
			if (_presentation.IsInstantiated(manifest.id)) {
				deliver(manifest.id);
			}
		}
	}

	void Runtime::OnViewGreeted(std::string_view a_viewId)
	{
		if (!_bridge) {
			return;
		}
		for (const auto* key : { "settings", "views", "diagnostics", "keybindings", "input-context", "i18n" }) {
			PublishPlatformState(key, a_viewId);
		}
		const std::string mod{ Ids::ModOf(a_viewId) };
		if (const auto* entries = _retainedState.Find(mod)) {
			for (const auto& entry : *entries) {
				_bridge->PublishState(a_viewId, mod, entry.key, entry.value);
			}
		}
		m_viewInputGrants.ResetPage(a_viewId);
	}

	void Runtime::OnProtocolFault(std::string_view a_viewId, std::string_view a_code, std::string_view a_message, const nlohmann::json& a_detail, bool a_viewFault)
	{

		if (_developerMode && _bridge && !a_viewId.empty()) {
			_bridge->Emit(a_viewId, "osfui.debug.error", nlohmann::json{
				{ "code", std::string(a_code) },
				{ "message", std::string(a_message) },
				{ "detail", a_detail },
			});
		}
		
		if (!a_viewFault || a_viewId.empty()) {
			return;
		}
		_runtimeHealth.ReportProtocolFault(a_viewId, a_code);
	}

    void Runtime::RegisterPlatformEndpoints(MessageBridge& a_bridge)
	{
		a_bridge.RegisterSend("close", [this](const nlohmann::json&, MessageBridge& a_b) {
			const std::string source(a_b.CurrentSource());
			if (_pendingViewOpen && *_pendingViewOpen == source) {
				CancelPendingOpen();
				return;
			}
			if (_presentation.Close(source)) {
				ApplyViewPresentationPolicy();
			}
		});
		a_bridge.RegisterSend("setVisible", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			const std::string src(a_b.CurrentSource());
			const bool visible = Json::Get(a_p, "visible", false);
			if (!visible && _pendingViewOpen && *_pendingViewOpen == src) {
				CancelPendingOpen();
			}
			const bool changed = visible ? BeginViewOpen(src) : _presentation.Close(src);
			if (changed) {
				ApplyViewPresentationPolicy();
			}
		});
		a_bridge.RegisterRequest("menu.open", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			std::string id = Json::Get(a_p, "view", "");
			if (id.empty()) {
				id = std::string(a_b.CurrentSource());
			}
			const auto* manifest = _views.Find(id);
			if (!manifest) {
				REX::WARN("Runtime: menu.open refused — '{}' was not discovered", id);
				a_b.Reject("unknown-view", "view was not discovered");
				return;
			}
			id = manifest->id;
			if (manifest->kind == ViewKind::Menu && manifest->capturesInput && _captureIntegrationInitialized && !_captureIntegrationAvailable) {
				REX::WARN("Runtime: menu.open refused — required input integration is unavailable");
				a_b.Reject("input-unavailable", "required input integration is unavailable");
				return;
			}
			EnqueueOpenView(std::move(id));
			a_b.Respond(nlohmann::json::object());
		});
		a_bridge.RegisterRequest("menu.close", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			std::string id = Json::Get(a_p, "view", "");
			if (id.empty()) {
				id = std::string(a_b.CurrentSource());
			}
			if (const auto* manifest = _views.Find(id)) {
				id = manifest->id;
			}
			bool cancelled = false;
			if (_pendingViewOpen && *_pendingViewOpen == id) {
				cancelled = CancelPendingOpen();
			}
			if (_presentation.Close(id)) {
				ApplyViewPresentationPolicy();
			} else if (!cancelled && !_presentation.IsInstantiated(id)) {
				a_b.Reject("unknown-view", "view is not instantiated");
				return;
			}
			a_b.Respond(nlohmann::json::object());
		});
		a_bridge.RegisterRequest("setViewHidden", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			std::string id = Json::Get(a_p, "view", "");
			if (id.empty()) {
				id = std::string(a_b.CurrentSource());
			}
			if (const auto* manifest = _views.Find(id)) {
				id = manifest->id;
			}
			if (!_presentation.IsInstantiated(id)) {
				a_b.Reject("unknown-view", "not an instantiated view");
				return;
			}

			const bool hidden = Json::Get(a_p, "hidden", false);
			bool changed = false;
			if (hidden) {
				if (_pendingViewOpen && *_pendingViewOpen == id) {
					CancelPendingOpen();
				}
				changed = _presentation.Close(id);
			} else {
				changed = BeginViewOpen(id);
			}
			if (changed) {
				ApplyViewPresentationPolicy();
			}
			a_b.Respond(nlohmann::json::object());
		});
		a_bridge.RegisterRequest("settings.captureKey", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			const auto requestedMod = Json::Get(a_p, "mod", "");
			const auto allowedMod = Ids::ResolveWritableMod(a_b.CurrentSource(), requestedMod);
			if (!allowedMod) {
				REX::WARN("Runtime: [content] view '{}' refused settings.captureKey for '{}' (not its own mod)", a_b.CurrentSource(), requestedMod);
				a_b.Reject("forbidden", "a view may only rebind its own mod's keys");
				return;
			}
			const std::string mod(*allowedMod);
			const std::string key = Json::Get(a_p, "key", "");
			if (_captureArmed.load()) {
				REX::WARN("Runtime: settings.captureKey rejected — a capture is already in progress ({}.{})", _captureMod, _captureKey);
				a_b.Reject("capture-busy", "a key capture is already in progress");
				return;
			}
			if (!_settings || _settings->Store().GetSettingType(mod, key) != "key") {
				REX::WARN("Runtime: settings.captureKey rejected — '{}.{}' is not a key-typed setting", mod.substr(0, 64), key.substr(0, 64));
				a_b.Reject("not-rebindable", "only a key-typed setting can be rebound");
				return;
			}
			RefreshKeyboardLabels("capture arm");
			_captureView = std::string(a_b.CurrentSource());
			_captureMod = mod;
			_captureKey = key;
			_captureArmed.store(true);
			REX::DEBUG("Runtime: armed key capture for {}.{} (from view '{}')", mod, key, _captureView);
			a_b.Respond(nlohmann::json{ { "armed", true }, { "mod", mod }, { "key", key } });
		});
		// Non-string arg elements are coerced so view can send `args: [1, 7]` without stringifying
		const auto papyrusArgs = [](const nlohmann::json& a_p) {
			std::vector<std::string> args;
			const auto* list = Json::GetArray(a_p, "args");
			if (!list) {
				return args;
			}
			args.reserve(list->size());
			for (const auto& e : *list) {
				if (e.is_string()) {
					args.push_back(e.get<std::string>());
				} else if (e.is_number_unsigned()) {
					args.push_back(std::to_string(e.get<std::uint64_t>()));
				} else if (e.is_number_integer()) {
					args.push_back(std::to_string(e.get<std::int64_t>()));
				} else if (e.is_number()) {
					args.push_back(std::to_string(e.get<double>()));
				} else if (e.is_boolean()) {
					args.emplace_back(e.get<bool>() ? "true" : "false");
				} else {
					args.emplace_back();  // null/object/array element -> ""
				}
			}
			return args;
		};
		a_bridge.RegisterSend("papyrus.call", [](const nlohmann::json& a_p, MessageBridge& a_b) {
			const std::string source(a_b.CurrentSource());
			const auto call = PapyrusCall::Parse(a_p);
			if (!call.ok) {
				a_b.ReportProtocolFault(source, call.code, call.message);
				return;
			}
			if (API::Papyrus::DispatchStaticFunction(call.script, call.function, call.args) !=
				API::Papyrus::StaticDispatchResult::kQueued) {
				a_b.ReportProtocolFault(source, "papyrus-unavailable", "Papyrus could not queue the GLOBAL function");
			}
		});
		a_bridge.RegisterSend("papyrus.send", [papyrusArgs](const nlohmann::json& a_p, MessageBridge& a_b) {
			const std::string source(a_b.CurrentSource());
			const std::string mod{ Ids::ModOf(source) };
			const std::string name = Json::Get(a_p, "name", "");
			if (name.empty()) {
				a_b.ReportProtocolFault(source, "invalid-request", "papyrus.send requires a non-empty 'name'");
				return;
			}
			API::Papyrus::OnViewAction(mod, name, papyrusArgs(a_p));
		});
		a_bridge.RegisterRequest("papyrus.request", [papyrusArgs](const nlohmann::json& a_p, MessageBridge& a_b) {
			const std::string source(a_b.CurrentSource());
			const std::string mod{ Ids::ModOf(source) };
			const std::string name = Json::Get(a_p, "name", "");
			if (mod.empty() || name.empty() || name.size() > 64) {
				a_b.Reject("invalid-request", "name must be a non-empty string of at most 64 characters");
				return;
			}
			const auto token = a_b.Defer();
			if (!API::Papyrus::OnViewRequest(mod, name, papyrusArgs(a_p), source, token)) {
				a_b.RejectTo(token, "papyrus-unavailable", "no Papyrus request listener is available");
				return;
			}
			// script settles it later through ReplyView*/RejectViewRequest.
		});
		a_bridge.RegisterRequest("ping", [](const nlohmann::json&, MessageBridge& a_b) {
			a_b.Respond(nlohmann::json::object());
		});
		a_bridge.RegisterSend("osfui.gamepadMode", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			const std::string src(a_b.CurrentSource());
			if (src.empty()) {
				return;
			}
			const auto modeName = Json::Get(a_p, "mode", std::string("default"));
			GamepadSession::Mode mode;
			if (modeName == "default") {
				mode = GamepadSession::Mode::kDefault;
			} else if (modeName == "buttons") {
				mode = GamepadSession::Mode::kButtons;
			} else if (modeName == "raw") {
				mode = GamepadSession::Mode::kRaw;
			} else {
				REX::WARN("Runtime: [content] view '{}' requested unknown gamepad mode '{}'", src, modeName);
				return;
			}
			m_viewInputGrants.SetGamepadMode(src, mode);
		});
		a_bridge.RegisterSend("osfui.gamepadRaw", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			const std::string src(a_b.CurrentSource());
			if (src.empty()) {
				return;
			}
			m_viewInputGrants.SetGamepadMode(src, Json::Get(a_p, "raw", false) ? GamepadSession::Mode::kRaw : GamepadSession::Mode::kDefault);
		});
		a_bridge.RegisterSend("osfui.handleBack", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			const std::string src(a_b.CurrentSource());
			if (src.empty()) {
				return;
			}
			const bool handle = Json::Get(a_p, "handle", false);
			std::string target = Json::Get(a_p, "view", "");
			if (handle && !target.empty()) {
				const auto* manifest = _views.Find(target);
				if (!manifest || manifest->kind != ViewKind::Menu || manifest->id == src) {
					REX::WARN("Runtime: [content] view '{}' requested invalid back target '{}'", src, target);
					target.clear();  // Preserve ordinary browser-owned Back as the safe fallback.
				} else {
					target = manifest->id;
				}
			}
			m_viewInputGrants.SetBackOwnership(src, handle, target);
		});
        a_bridge.RegisterRequest("osfui.setViewAutoStart", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
            if(a_b.CurrentSource() != kSettingsViewId) {
                a_b.Reject("forbidden", "view auto-start is set from OSF UI's built-in settings view");
                return;
            }
            const auto view = Json::Get(a_p, "view", "");
            const auto enabled = a_p.find("enabled");
            if(view.empty() || enabled == a_p.end() || !enabled->is_boolean()) {
                a_b.Reject("invalid-payload", "expected { view: string, enabled: boolean }");
                return;
            }
            const auto* manifest = _views.Find(view);
            if(!manifest) {
                a_b.Reject("unknown-view", "not a discovered view");
                return;
            }
            if(!HudAutoStartEligible(*manifest)) {
                a_b.Reject("not-configurable", "auto-start is settable only for catalog-visible HUDs");
                return;
            }
            if(!_viewPolicy.SetHudAutoStart(view, enabled->get<bool>())) { a_b.Reject("persistence-failed", "the choice could not be saved, so it was not applied" );
                return;
            }
            REX::INFO("Runtime: HUD '{}' auto-start set to {} (next launch)", view,enabled->get<bool>());
            BroadcastViewsData();
            a_b.Respond(nlohmann::json::object());
        });
	}
}
