#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "OSFUI_API.h"

namespace OSFUI::Compat::V1
{
	inline constexpr std::uint32_t kBridgeAPIVersion = (1u << 16) | 8u;
	[[nodiscard]] inline constexpr bool SupportsRequestedAbi(std::uint32_t a_version)
	{
		return (a_version >> 16) == 1u;
	}

	using CommandFn = void (*)(const char*, const char*, const char*, void*) noexcept;
	using ReadyFn = void (*)(void*) noexcept;
	using SettingChangedFn = void (*)(const char*, const char*, const char*, void*) noexcept;
	using HotkeyFn = void (*)(const char*, const char*, void*) noexcept;

	struct Request
	{
		using RespondFn = void (*)(std::uint64_t, const char*, const char*) noexcept;
		using RejectFn = void (*)(std::uint64_t, const char*, const char*) noexcept;

		const char* command{ nullptr };
		const char* payloadJson{ nullptr };
		const char* sourceViewId{ nullptr };

		void Respond(const char* a_payloadJson) const noexcept
		{
			if (_respond) _respond(_token, nullptr, a_payloadJson);
		}
		void Respond(const char* a_type, const char* a_payloadJson) const noexcept
		{
			if (_respond) _respond(_token, a_type, a_payloadJson);
		}
		void Reject(const char* a_code, const char* a_message = "") const noexcept
		{
			if (_reject) _reject(_token, a_code, a_message);
		}

		std::uint64_t _token{ 0 };
		RespondFn _respond{ nullptr };
		RejectFn _reject{ nullptr };
	};
	using RequestFn = void (*)(const Request&, void*) noexcept;
	static_assert(std::is_standard_layout_v<Request> && std::is_trivially_copyable_v<Request>);
	static_assert(sizeof(Request) == sizeof(API::Request));

	// Frozen final 1.x vtable. Do not reorder: callers compiled against ABI
	// 1.0 through 1.8 invoke the prefix they knew at build time.
	struct IOSFUIBridge
	{
		virtual std::uint32_t GetInterfaceVersion() = 0;
		virtual void GetPluginVersion(std::uint32_t&, std::uint32_t&, std::uint32_t&) = 0;
		virtual const char* GetBridgeProtocolVersion() = 0;
		virtual bool IsBridgeReady() = 0;
		virtual void RegisterCommand(const char*, CommandFn, void*) = 0;
		virtual void UnregisterCommand(const char*) = 0;
		virtual bool SendToWeb(const char*, const char*, const char*) = 0;
		virtual void SetReadyCallback(ReadyFn, void*) = 0;
		virtual bool RequestMenu(const char*, bool) = 0;
		virtual std::uint32_t SubscribeSettings(const char*, SettingChangedFn, void*) = 0;
		virtual void UnsubscribeSettings(std::uint32_t) = 0;
		virtual bool GetSettingBool(const char*, const char*, bool*) = 0;
		virtual bool GetSettingInt(const char*, const char*, std::int64_t*) = 0;
		virtual bool GetSettingFloat(const char*, const char*, double*) = 0;
		virtual std::uint32_t GetSettingString(const char*, const char*, char*, std::uint32_t) = 0;
		virtual bool RegisterSettingsSchema(const char*) = 0;
		virtual void UnregisterSettingsSchema(const char*) = 0;
		virtual std::uint32_t SubscribeHotkey(const char*, const char*, HotkeyFn, void*) = 0;
		virtual void UnsubscribeHotkey(std::uint32_t) = 0;
		virtual bool RegisterView(const char*) = 0;
		// Frozen ABI 1.7 slots. Reporting was removed; the adapter keeps the
		// vtable shape and returns false to old binaries.
		virtual bool ReportIssue(const char*, const char*, const char*, std::uint32_t,
			const char*, const char*) = 0;
		virtual bool ClearIssue(const char*, const char*) = 0;
		virtual bool ClearIssuesExcept(const char*, const char*) = 0;
		virtual void RegisterRequest(const char*, RequestFn, void*) = 0;
		virtual void UnregisterRequest(const char*) = 0;
		virtual bool SetViewState(const char*, const char*, const char*) = 0;

	protected:
		~IOSFUIBridge() = default;
	};

	class NativeBridge final : public IOSFUIBridge
	{
	public:
		static NativeBridge& Get();

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
		bool ReportIssue(const char*, const char*, const char*, std::uint32_t,
			const char*, const char*) override { return false; }
		bool ClearIssue(const char*, const char*) override { return false; }
		bool ClearIssuesExcept(const char*, const char*) override { return false; }
		void RegisterRequest(const char*, RequestFn, void*) override;
		void UnregisterRequest(const char*) override;
		bool SetViewState(const char*, const char*, const char*) override;

	private:
		struct RequestRegistration
		{
			RequestFn fn{ nullptr };
			void* user{ nullptr };
		};
		static void RequestThunk(const API::Request&, void*) noexcept;

		std::mutex _mutex;
		std::unordered_map<std::string, std::unique_ptr<RequestRegistration>> _requests;
		// BridgeApi applies unregisters on the next main tick; retaining retired
		// thunks for process life makes an in-flight callback unconditionally safe.
		std::vector<std::unique_ptr<RequestRegistration>> _retiredRequests;
	};
}
