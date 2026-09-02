#include "Runtime/Runtime.h"

#include <format>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

#include "API/PapyrusApi.h"
#include "API/PapyrusCall.h"
#include "Core/Ids.h"
#include "Core/Json.h"

namespace OSFUI
{
	namespace
	{
		std::optional<std::vector<API::Papyrus::Value>> ParsePapyrusArgs(const nlohmann::json& a_payload, std::string& a_error)
		{
			std::vector<API::Papyrus::Value> args;
			const auto it = a_payload.find("args");
			if (it == a_payload.end()) return args;
			if (!it->is_array()) {
				a_error = "payload.args must be an array";
				return std::nullopt;
			}
			constexpr std::size_t kMaxPapyrusArgs = 64;
			if (it->size() > kMaxPapyrusArgs) {
				a_error = "payload.args may contain at most 64 values";
				return std::nullopt;
			}
			args.reserve(it->size());
			for (const auto& value : *it) {
				if (value.is_null()) {
					args.emplace_back(std::monostate{});
				} else if (value.is_boolean()) {
					args.emplace_back(value.get<bool>());
				} else if (value.is_number_unsigned()) {
					const auto number = value.get<std::uint64_t>();
					if (number > static_cast<std::uint64_t>(INT32_MAX)) {
						a_error = "integer args must fit the Papyrus signed 32-bit range";
						return std::nullopt;
					}
					args.emplace_back(static_cast<std::int32_t>(number));
				} else if (value.is_number_integer()) {
					const auto number = value.get<std::int64_t>();
					if (number < INT32_MIN || number > INT32_MAX) {
						a_error = "integer args must fit the Papyrus signed 32-bit range";
						return std::nullopt;
					}
					args.emplace_back(static_cast<std::int32_t>(number));
				} else if (value.is_number_float()) {
					const auto number = value.get<double>();
					if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max()) {
						a_error = "float args must be finite Papyrus float values";
						return std::nullopt;
					}
					args.emplace_back(static_cast<float>(number));
				} else if (value.is_string()) {
					args.emplace_back(value.get<std::string>());
				} else if (value.is_object()) {
					const auto form = value.find("formId");
					if (form == value.end() || (!form->is_number_integer() && !form->is_number_unsigned())) {
						a_error = "object args must be serialized Forms with an unsigned 32-bit formId";
						return std::nullopt;
					}
					std::uint32_t formId = 0;
					if (form->is_number_unsigned()) {
						const auto number = form->get<std::uint64_t>();
						if (number > UINT32_MAX) {
							a_error = "Form args require an unsigned 32-bit formId";
							return std::nullopt;
						}
						formId = static_cast<std::uint32_t>(number);
					} else {
						const auto number = form->get<std::int64_t>();
						if (number < 0 || number > UINT32_MAX) {
							a_error = "Form args require an unsigned 32-bit formId";
							return std::nullopt;
						}
						formId = static_cast<std::uint32_t>(number);
					}
					args.emplace_back(API::Papyrus::FormValue{ formId });
				} else {
					a_error = "args support only null, bool, int, float, string, and serialized Form values";
					return std::nullopt;
				}
			}
			return args;
		}
	}

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
		PublishPlatformState("views", a_viewId);
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
			if (CancelPendingOpen(source)) {
				return;
			}
			if (_presentation.Close(source)) {
				ApplyViewPresentationPolicy();
			}
		});
		a_bridge.RegisterSend("setVisible", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			const std::string src(a_b.CurrentSource());
			const bool visible = Json::Get(a_p, "visible", false);
			if (!visible) {
				CancelPendingOpen(src);
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
			cancelled = CancelPendingOpen(id);
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
				CancelPendingOpen(id);
				changed = _presentation.Close(id);
			} else {
				changed = BeginViewOpen(id);
			}
			if (changed) {
				ApplyViewPresentationPolicy();
			}
			a_b.Respond(nlohmann::json::object());
		});
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
		a_bridge.SetEndpointFallback(
			[](std::string_view a_sourceViewId, std::string_view a_name) {
				const auto endpoint = API::Papyrus::ResolveViewEndpoint(Ids::ModOf(a_sourceViewId), a_name);
				switch (endpoint.kind) {
				case API::Papyrus::ViewEndpointKind::kSend:
					return MessageBridge::FallbackEndpointKind::kSend;
				case API::Papyrus::ViewEndpointKind::kRequest:
					return MessageBridge::FallbackEndpointKind::kRequest;
				default:
					return MessageBridge::FallbackEndpointKind::kNone;
				}
			},
			[](std::string_view a_name, const nlohmann::json& a_payload, MessageBridge& a_b) {
				const std::string source(a_b.CurrentSource());
				const auto endpoint = API::Papyrus::ResolveViewEndpoint(Ids::ModOf(source), a_name);
				std::string error;
				auto args = ParsePapyrusArgs(a_payload, error);
				if (!args) {
					a_b.ReportProtocolFault(source, "invalid-payload", error, { { "name", a_name } });
					return;
				}
				if (endpoint.kind != API::Papyrus::ViewEndpointKind::kSend || !API::Papyrus::OnViewSend(endpoint.modId, endpoint.name, *args, source)) {
					a_b.ReportProtocolFault(source, "papyrus-unavailable", "Papyrus send endpoint is no longer available", { { "name", a_name } }, false);
				}
			},
			[](std::string_view a_name, const nlohmann::json& a_payload, MessageBridge& a_b) {
				const std::string source(a_b.CurrentSource());
				const auto endpoint = API::Papyrus::ResolveViewEndpoint(Ids::ModOf(source), a_name);
				std::string error;
				auto args = ParsePapyrusArgs(a_payload, error);
				if (!args) {
					a_b.Reject("invalid-payload", error);
					return;
				}
				if (endpoint.kind != API::Papyrus::ViewEndpointKind::kRequest) {
					a_b.Reject("papyrus-unavailable", "Papyrus request endpoint is no longer available");
					return;
				}
				const auto token = a_b.Defer();
				if (!API::Papyrus::OnViewRequest(endpoint.modId, endpoint.name, *args, source, token)) {
					a_b.RejectTo(token, "papyrus-unavailable", "Papyrus request endpoint is no longer available");
				}
			});
		a_bridge.RegisterRequest("ping", [](const nlohmann::json&, MessageBridge& a_b) {
			a_b.Respond(nlohmann::json::object());
		});
		a_bridge.RegisterSend("osfui.relativePointer", [this](const nlohmann::json& a_p, MessageBridge& a_b) {
			const std::string src(a_b.CurrentSource());
			const auto activeValue = a_p.find("active");
			if (src.empty() || activeValue == a_p.end() || !activeValue->is_boolean()) {
				a_b.ReportProtocolFault(src, "invalid-payload",
					"osfui.relativePointer expects { active: boolean }");
				return;
			}
			EnqueueRelativePointerCapture(src, activeValue->get<bool>());
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
	}
}
