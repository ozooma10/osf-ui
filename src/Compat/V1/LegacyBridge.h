#pragma once

#include "OSFUI_API.h"

namespace OSFUI::API::Legacy
{
	class Bridge final : public OSFUI::API::IOSFUIBridge
	{
	public:
		static Bridge& Get();

		std::uint32_t GetInterfaceVersion() override;
		void GetPluginVersion(std::uint32_t&, std::uint32_t&, std::uint32_t&) override;
		const char* GetBridgeProtocolVersion() override;
		bool IsBridgeReady() override;
		void RegisterCommand(const char*, CommandFn, void*) override;
		void UnregisterCommand(const char*) override;
		bool SendToWeb(const char*, const char*, const char*) override;
		void SetReadyCallback(ReadyFn, void*) override;
		bool RequestMenu(const char*, bool) override;
		std::uint32_t SubscribeSettings(const char*, SettingChangedFn, void*) override;
		void UnsubscribeSettings(std::uint32_t) override;
		bool GetSettingBool(const char*, const char*, bool*) override;
		bool GetSettingInt(const char*, const char*, std::int64_t*) override;
		bool GetSettingFloat(const char*, const char*, double*) override;
		std::uint32_t GetSettingString(const char*, const char*, char*, std::uint32_t) override;
		bool RegisterSettingsSchema(const char*) override;
		void UnregisterSettingsSchema(const char*) override;
		std::uint32_t SubscribeHotkey(const char*, const char*, HotkeyFn, void*) override;
		void UnsubscribeHotkey(std::uint32_t) override;
		bool RegisterView(const char*) override;
		bool ReportIssue(const char*, const char*, const char*, std::uint32_t, const char*, const char*) override;
		bool ClearIssue(const char*, const char*) override;
		bool ClearIssuesExcept(const char*, const char*) override;
		void RegisterRequest(const char*, RequestFn, void*) override;
		void UnregisterRequest(const char*) override;

	private:
		Bridge() = default;
	};
}
