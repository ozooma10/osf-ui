// OSF UI legacy native API (ABI 1.7).
#pragma once

#include <cstdint>
#include <type_traits>
#include "REX/W32/KERNEL32.h"

static_assert(sizeof(void*) == 8, "OSFUI_API requires x64");

namespace OSFUI::API
{
	inline constexpr std::uint32_t kBridgeAPIVersion = 0x00010007u;
	inline constexpr std::uint32_t kBridgeAPIMajor = 1;
	inline constexpr std::uint32_t kBridgeAPIMinor = 7;
	inline constexpr const wchar_t* kModuleName = L"OSFUI.dll";
	inline constexpr const char* kRequestExportName = "OSFUI_RequestBridge";

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

		void Respond(const char* a_json) const noexcept
		{
			if (_respond) _respond(_token, nullptr, a_json);
		}

		void Respond(const char* a_type, const char* a_json) const noexcept
		{
			if (_respond) _respond(_token, a_type, a_json);
		}

		void Reject(const char* a_code, const char* a_message = "") const noexcept
		{
			if (_reject) _reject(_token, a_code, a_message);
		}

		std::uint64_t _token{ 0 };
		RespondFn _respond{ nullptr };
		RejectFn _reject{ nullptr };
	};

	static_assert(std::is_standard_layout_v<Request> && std::is_trivially_copyable_v<Request>);
	using RequestFn = void (*)(const Request&, void*) noexcept;

	enum class IssueSeverity : std::uint32_t
	{
		kWarning,
		kError,
	};

	// Frozen v1.5.0 vtable.
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
		virtual bool ReportIssue(const char*, const char*, const char*, std::uint32_t, const char*, const char*) = 0;
		virtual bool ClearIssue(const char*, const char*) = 0;
		virtual bool ClearIssuesExcept(const char*, const char*) = 0;
		virtual void RegisterRequest(const char*, RequestFn, void*) = 0;
		virtual void UnregisterRequest(const char*) = 0;

	protected:
		~IOSFUIBridge() = default;
	};

	using RequestBridge_t = IOSFUIBridge* (*)(std::uint32_t) noexcept;

	inline IOSFUIBridge* RequestBridge(std::uint32_t a_version = kBridgeAPIVersion) noexcept
	{
		const auto module = REX::W32::GetModuleHandleW(kModuleName);
		if (!module) return nullptr;
		const auto fn = reinterpret_cast<RequestBridge_t>(REX::W32::GetProcAddress(module, kRequestExportName));

		return fn ? fn(a_version) : nullptr;
	}

	enum class Feature : std::uint32_t
	{
		kCommands = 0,
		kRequestMenu = 1,
		kSettings = 2,
		kDeliveryGuarantee = 3,
		kHotkeys = 4,
		kRegisterView = 5,
		kCommandShape = 6,
		kDiagnostics = 7,
		kRequests = 7,
	};

	class Client
	{
	public:
		bool Init(std::uint32_t a_version = kBridgeAPIVersion) noexcept { return Attach(RequestBridge(a_version)); }
		bool Attach(IOSFUIBridge* a_bridge) noexcept
		{
			_bridge = a_bridge;
			_minor = _bridge ? (_bridge->GetInterfaceVersion() & 0xFFFFu) : 0;
			return _bridge != nullptr;
		}

		[[nodiscard]] explicit operator bool() const noexcept { return _bridge != nullptr; }
		[[nodiscard]] bool IsConnected() const noexcept { return _bridge != nullptr; }
		[[nodiscard]] bool Has(Feature a_feature) const noexcept
		{
			return _bridge && _minor >= static_cast<std::uint32_t>(a_feature);
		}
		[[nodiscard]] IOSFUIBridge* Raw() const noexcept { return _bridge; }
		[[nodiscard]] std::uint32_t GetInterfaceVersion() const noexcept
		{
			return _bridge ? _bridge->GetInterfaceVersion() : 0;
		}
		void GetPluginVersion(std::uint32_t& a_major, std::uint32_t& a_minor, std::uint32_t& a_patch) const noexcept
		{
			if (_bridge) {
				_bridge->GetPluginVersion(a_major, a_minor, a_patch);
			} else {
				a_major = a_minor = a_patch = 0;
			}
		}
		[[nodiscard]] const char* GetBridgeProtocolVersion() const noexcept
		{
			return _bridge ? _bridge->GetBridgeProtocolVersion() : "";
		}
		[[nodiscard]] bool IsBridgeReady() const noexcept
		{
			return _bridge && _bridge->IsBridgeReady();
		}

		void RegisterCommand(const char* a_name, CommandFn a_fn, void* a_user) const noexcept
		{
			if (_bridge) _bridge->RegisterCommand(a_name, a_fn, a_user);
		}
		void UnregisterCommand(const char* a_name) const noexcept
		{
			if (_bridge) _bridge->UnregisterCommand(a_name);
		}
		bool SendToWeb(const char* a_view, const char* a_type, const char* a_json) const noexcept
		{
			return _bridge && _bridge->SendToWeb(a_view, a_type, a_json);
		}
		void SetReadyCallback(ReadyFn a_fn, void* a_user) const noexcept
		{
			if (_bridge) _bridge->SetReadyCallback(a_fn, a_user);
		}
		bool RequestMenu(const char* a_view, bool a_open) const noexcept
		{
			return Has(Feature::kRequestMenu) && _bridge->RequestMenu(a_view, a_open);
		}
		std::uint32_t SubscribeSettings(const char* a_mod, SettingChangedFn a_fn, void* a_user) const noexcept
		{
			return Has(Feature::kSettings) ? _bridge->SubscribeSettings(a_mod, a_fn, a_user) : 0;
		}
		void UnsubscribeSettings(std::uint32_t a_token) const noexcept
		{
			if (Has(Feature::kSettings)) _bridge->UnsubscribeSettings(a_token);
		}
		bool GetSettingBool(const char* a_mod, const char* a_key, bool* a_out) const noexcept
		{
			return Has(Feature::kSettings) && _bridge->GetSettingBool(a_mod, a_key, a_out);
		}
		bool GetSettingInt(const char* a_mod, const char* a_key, std::int64_t* a_out) const noexcept
		{
			return Has(Feature::kSettings) && _bridge->GetSettingInt(a_mod, a_key, a_out);
		}
		bool GetSettingFloat(const char* a_mod, const char* a_key, double* a_out) const noexcept
		{
			return Has(Feature::kSettings) && _bridge->GetSettingFloat(a_mod, a_key, a_out);
		}
		std::uint32_t GetSettingString(const char* a_mod, const char* a_key, char* a_buf, std::uint32_t a_len) const noexcept
		{
			return Has(Feature::kSettings) ? _bridge->GetSettingString(a_mod, a_key, a_buf, a_len) : 0;
		}
		bool RegisterSettingsSchema(const char* a_json) const noexcept
		{
			return Has(Feature::kSettings) && _bridge->RegisterSettingsSchema(a_json);
		}
		void UnregisterSettingsSchema(const char* a_mod) const noexcept
		{
			if (Has(Feature::kSettings)) _bridge->UnregisterSettingsSchema(a_mod);
		}
		std::uint32_t SubscribeHotkey(const char* a_mod, const char* a_key, HotkeyFn a_fn, void* a_user) const noexcept
		{
			return Has(Feature::kHotkeys) ? _bridge->SubscribeHotkey(a_mod, a_key, a_fn, a_user) : 0;
		}
		void UnsubscribeHotkey(std::uint32_t a_token) const noexcept
		{
			if (Has(Feature::kHotkeys)) _bridge->UnsubscribeHotkey(a_token);
		}
		bool RegisterView(const char* a_view) const noexcept
		{
			return Has(Feature::kRegisterView) && _bridge->RegisterView(a_view);
		}
		bool ReportIssue(const char* a_mod, const char* a_id, const char* a_code, IssueSeverity a_severity,
			const char* a_subject = "", const char* a_context = nullptr) const noexcept
		{
			return Has(Feature::kDiagnostics) && _bridge->ReportIssue(a_mod, a_id, a_code,
				static_cast<std::uint32_t>(a_severity), a_subject, a_context);
		}
		bool ClearIssue(const char* a_mod, const char* a_id) const noexcept
		{
			return Has(Feature::kDiagnostics) && _bridge->ClearIssue(a_mod, a_id);
		}
		bool ClearIssuesExcept(const char* a_mod, const char* a_keep) const noexcept
		{
			return Has(Feature::kDiagnostics) && _bridge->ClearIssuesExcept(a_mod, a_keep);
		}
		void RegisterRequest(const char* a_name, RequestFn a_fn, void* a_user) const noexcept
		{
			if (Has(Feature::kRequests)) _bridge->RegisterRequest(a_name, a_fn, a_user);
		}
		void UnregisterRequest(const char* a_name) const noexcept
		{
			if (Has(Feature::kRequests)) _bridge->UnregisterRequest(a_name);
		}

	private:
		IOSFUIBridge* _bridge{ nullptr };
		std::uint32_t _minor{ 0 };
	};
}
