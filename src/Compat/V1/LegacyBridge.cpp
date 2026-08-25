#include "Compat/V1/LegacyBridge.h"

#include "API/BridgeApi.h"

namespace OSFUI::API::Legacy
{
	Bridge& Bridge::Get()
	{
		static Bridge* const instance = new Bridge;
		return *instance;
	}

	std::uint32_t Bridge::GetInterfaceVersion() { return OSFUI::API::kBridgeAPIVersion; }

	void Bridge::GetPluginVersion(std::uint32_t& a, std::uint32_t& b, std::uint32_t& c)
	{
		BridgeApi::Get().GetPluginVersion(a, b, c);
	}

	const char* Bridge::GetBridgeProtocolVersion() { return BridgeApi::Get().GetBridgeProtocolVersion(); }
	bool Bridge::IsBridgeReady() { return BridgeApi::Get().IsBridgeReady(); }

	void Bridge::RegisterCommand(const char* a, CommandFn b, void* c) { BridgeApi::Get().RegisterCommand(a, b, c); }
	void Bridge::UnregisterCommand(const char* a) { BridgeApi::Get().UnregisterCommand(a); }
	bool Bridge::SendToWeb(const char* a, const char* b, const char* c) { return BridgeApi::Get().SendToWeb(a, b, c); }
	void Bridge::SetReadyCallback(ReadyFn a, void* b) { BridgeApi::Get().SetReadyCallback(a, b); }
	bool Bridge::RequestMenu(const char* a, bool b) { return BridgeApi::Get().RequestMenu(a, b); }

	std::uint32_t Bridge::SubscribeSettings(const char* a, SettingChangedFn b, void* c)
	{
		return BridgeApi::Get().SubscribeSettings(a, b, c);
	}

	void Bridge::UnsubscribeSettings(std::uint32_t a) { BridgeApi::Get().UnsubscribeSettings(a); }
	bool Bridge::GetSettingBool(const char* a, const char* b, bool* c) { return BridgeApi::Get().GetSettingBool(a, b, c); }
	bool Bridge::GetSettingInt(const char* a, const char* b, std::int64_t* c) { return BridgeApi::Get().GetSettingInt(a, b, c); }
	bool Bridge::GetSettingFloat(const char* a, const char* b, double* c) { return BridgeApi::Get().GetSettingFloat(a, b, c); }

	std::uint32_t Bridge::GetSettingString(const char* a, const char* b, char* c, std::uint32_t d)
	{
		return BridgeApi::Get().GetSettingString(a, b, c, d);
	}

	bool Bridge::RegisterSettingsSchema(const char* a) { return BridgeApi::Get().RegisterSettingsSchema(a); }
	void Bridge::UnregisterSettingsSchema(const char* a) { BridgeApi::Get().UnregisterSettingsSchema(a); }

	std::uint32_t Bridge::SubscribeHotkey(const char* a, const char* b, HotkeyFn c, void* d)
	{
		return BridgeApi::Get().SubscribeHotkey(a, b, c, d);
	}

	void Bridge::UnsubscribeHotkey(std::uint32_t a) { BridgeApi::Get().UnsubscribeHotkey(a); }
	bool Bridge::RegisterView(const char* a) { return BridgeApi::Get().RegisterView(a); }

	bool Bridge::ReportIssue(const char* a, const char* b, const char* c, std::uint32_t d,
		const char* e, const char* f)
	{
		return BridgeApi::Get().ReportIssue(a, b, c, d, e, f);
	}

	bool Bridge::ClearIssue(const char* a, const char* b) { return BridgeApi::Get().ClearIssue(a, b); }
	bool Bridge::ClearIssuesExcept(const char* a, const char* b) { return BridgeApi::Get().ClearIssuesExcept(a, b); }
	void Bridge::RegisterRequest(const char* a, RequestFn b, void* c) { BridgeApi::Get().RegisterRequest(a, b, c); }
	void Bridge::UnregisterRequest(const char* a) { BridgeApi::Get().UnregisterRequest(a); }
}
