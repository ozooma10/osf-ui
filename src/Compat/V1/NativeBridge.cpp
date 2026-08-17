#include "Compat/V1/NativeBridge.h"

#include "API/BridgeApi.h"

namespace OSFUI::Compat::V1
{
	NativeBridge& NativeBridge::Get()
	{
		static NativeBridge* const instance = new NativeBridge;
		return *instance;
	}

	std::uint32_t NativeBridge::GetInterfaceVersion() { return kBridgeAPIVersion; }
	void NativeBridge::GetPluginVersion(std::uint32_t& a, std::uint32_t& b, std::uint32_t& c)
	{
		API::BridgeApi::Get().GetPluginVersion(a, b, c);
	}
	const char* NativeBridge::GetBridgeProtocolVersion()
	{
		return API::BridgeApi::Get().GetBridgeProtocolVersion();
	}
	bool NativeBridge::IsBridgeReady() { return API::BridgeApi::Get().IsBridgeReady(); }
	void NativeBridge::RegisterCommand(const char* a_name, CommandFn a_fn, void* a_user)
	{
		API::BridgeApi::Get().RegisterLegacyCommand(a_name, a_fn, a_user);
	}
	void NativeBridge::UnregisterCommand(const char* a_name)
	{
		API::BridgeApi::Get().UnregisterLegacyCommand(a_name);
	}
	bool NativeBridge::SendToWeb(const char* a_view, const char* a_type, const char* a_json)
	{
		return API::BridgeApi::Get().SendToWeb(a_view, a_type, a_json);
	}
	void NativeBridge::SetReadyCallback(ReadyFn a_fn, void* a_user)
	{
		API::BridgeApi::Get().SetReadyCallback(a_fn, a_user);
	}
	bool NativeBridge::RequestMenu(const char* a_view, bool a_open)
	{
		return API::BridgeApi::Get().RequestMenu(a_view, a_open);
	}
	std::uint32_t NativeBridge::SubscribeSettings(const char* a_mod, SettingChangedFn a_fn, void* a_user)
	{
		return API::BridgeApi::Get().SubscribeSettings(a_mod, a_fn, a_user);
	}
	void NativeBridge::UnsubscribeSettings(std::uint32_t a_token)
	{
		API::BridgeApi::Get().UnsubscribeSettings(a_token);
	}
	bool NativeBridge::GetSettingBool(const char* a_mod, const char* a_key, bool* a_out)
	{
		return API::BridgeApi::Get().GetSettingBool(a_mod, a_key, a_out);
	}
	bool NativeBridge::GetSettingInt(const char* a_mod, const char* a_key, std::int64_t* a_out)
	{
		return API::BridgeApi::Get().GetSettingInt(a_mod, a_key, a_out);
	}
	bool NativeBridge::GetSettingFloat(const char* a_mod, const char* a_key, double* a_out)
	{
		return API::BridgeApi::Get().GetSettingFloat(a_mod, a_key, a_out);
	}
	std::uint32_t NativeBridge::GetSettingString(const char* a_mod, const char* a_key,
		char* a_buf, std::uint32_t a_len)
	{
		return API::BridgeApi::Get().GetSettingString(a_mod, a_key, a_buf, a_len);
	}
	bool NativeBridge::RegisterSettingsSchema(const char* a_schema)
	{
		return API::BridgeApi::Get().RegisterSettingsSchema(a_schema);
	}
	void NativeBridge::UnregisterSettingsSchema(const char* a_mod)
	{
		API::BridgeApi::Get().UnregisterSettingsSchema(a_mod);
	}
	std::uint32_t NativeBridge::SubscribeHotkey(const char* a_mod, const char* a_key,
		HotkeyFn a_fn, void* a_user)
	{
		return API::BridgeApi::Get().SubscribeHotkey(a_mod, a_key, a_fn, a_user);
	}
	void NativeBridge::UnsubscribeHotkey(std::uint32_t a_token)
	{
		API::BridgeApi::Get().UnsubscribeHotkey(a_token);
	}
	bool NativeBridge::RegisterView(const char* a_view)
	{
		return API::BridgeApi::Get().RegisterView(a_view);
	}
	void NativeBridge::RequestThunk(const API::Request& a_request, void* a_user) noexcept
	{
		const auto* reg = static_cast<const RequestRegistration*>(a_user);
		if (!reg || !reg->fn) return;
		Request legacy;
		legacy.command = a_request.command;
		legacy.payloadJson = a_request.payloadJson;
		legacy.sourceViewId = a_request.sourceViewId;
		legacy._token = a_request._token;
		legacy._respond = a_request._respond;
		legacy._reject = a_request._reject;
		reg->fn(legacy, reg->user);
	}

	void NativeBridge::RegisterRequest(const char* a_name, RequestFn a_fn, void* a_user)
	{
		if (!a_name || !a_fn) return;
		auto reg = std::make_unique<RequestRegistration>(RequestRegistration{ a_fn, a_user });
		std::lock_guard lock(_mutex);
		if (_requests.contains(a_name)) return;
		if (!API::BridgeApi::Get().RegisterLegacyRequest(a_name, &RequestThunk, reg.get())) return;
		_requests.emplace(a_name, std::move(reg));
	}

	void NativeBridge::UnregisterRequest(const char* a_name)
	{
		if (!a_name) return;
		std::lock_guard lock(_mutex);
		const auto it = _requests.find(a_name);
		if (it == _requests.end()) return;
		API::BridgeApi::Get().UnregisterLegacyRequest(a_name);
		_retiredRequests.push_back(std::move(it->second));
		_requests.erase(it);
	}

	bool NativeBridge::SetViewState(const char* a_mod, const char* a_key, const char* a_json)
	{
		return API::BridgeApi::Get().SetViewState(a_mod, a_key, a_json);
	}
}
