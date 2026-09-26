// Frozen v1.6.0 native ABI 1.7. Keep every slot and payload layout unchanged.
#pragma once
#include <cstdint>
#include <type_traits>
namespace OSFUI::Compat::V1
{
	using CommandFn = void (*)(const char* a_command,
	                           const char* a_payloadJson,
	                           const char* a_sourceViewId,
	                           void*       a_user) noexcept;
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
		RespondFn     _respond{ nullptr };
		RejectFn      _reject{ nullptr };
	};
	using RequestFn = void (*)(const Request& a_request, void* a_user) noexcept;
	static_assert(std::is_standard_layout_v<Request> && std::is_trivially_copyable_v<Request>);
	using ReadyFn = void (*)(void* a_user) noexcept;
	using SettingChangedFn = void (*)(const char* a_modId,
	                                  const char* a_key,
	                                  const char* a_valueJson,
	                                  void*       a_user) noexcept;
	using HotkeyFn = void (*)(const char* a_modId,
	                          const char* a_key,
	                          void*       a_user) noexcept;
	enum class IssueSeverity : std::uint32_t
	{
		kWarning = 0,
		kError = 1,
	};
	struct IOSFUIBridge
	{
		virtual std::uint32_t GetInterfaceVersion() = 0;
		virtual void          GetPluginVersion(std::uint32_t& a_major,
		                                       std::uint32_t& a_minor,
		                                       std::uint32_t& a_patch) = 0;
		virtual const char*   GetBridgeProtocolVersion() = 0;
		virtual bool          IsBridgeReady() = 0;
		virtual void RegisterCommand(const char* a_command, CommandFn a_handler, void* a_user) = 0;
		virtual void UnregisterCommand(const char* a_command) = 0;
		virtual bool SendToWeb(const char* a_viewId, const char* a_type, const char* a_payloadJson) = 0;
		virtual void SetReadyCallback(ReadyFn a_callback, void* a_user) = 0;
		virtual bool RequestMenu(const char* a_viewId, bool a_open) = 0;
		virtual std::uint32_t SubscribeSettings(const char* a_modId, SettingChangedFn a_fn, void* a_user) = 0;
		virtual void          UnsubscribeSettings(std::uint32_t a_token) = 0;
		virtual bool GetSettingBool(const char* a_modId, const char* a_key, bool* a_out) = 0;
		virtual bool GetSettingInt(const char* a_modId, const char* a_key, std::int64_t* a_out) = 0;
		virtual bool GetSettingFloat(const char* a_modId, const char* a_key, double* a_out) = 0;
		virtual std::uint32_t GetSettingString(const char* a_modId, const char* a_key, char* a_buf, std::uint32_t a_bufLen) = 0;
		virtual bool RegisterSettingsSchema(const char* a_schemaJson) = 0;
		virtual void UnregisterSettingsSchema(const char* a_modId) = 0;
		virtual std::uint32_t SubscribeHotkey(const char* a_modId, const char* a_key,
		                                      HotkeyFn a_fn, void* a_user) = 0;
		virtual void          UnsubscribeHotkey(std::uint32_t a_token) = 0;
		virtual bool RegisterView(const char* a_viewId) = 0;
		virtual bool ReportIssue(const char*   a_modId,
		                         const char*   a_id,
		                         const char*   a_code,
		                         std::uint32_t a_severity,
		                         const char*   a_subject,
		                         const char*   a_contextJson) = 0;
		virtual bool ClearIssue(const char* a_modId, const char* a_id) = 0;
		virtual bool ClearIssuesExcept(const char* a_modId, const char* a_keepIdsJson) = 0;
		virtual void RegisterRequest(const char* a_name, RequestFn a_handler, void* a_user) = 0;
		virtual void UnregisterRequest(const char* a_name) = 0;
	protected:
		~IOSFUIBridge() = default;
	};
}
