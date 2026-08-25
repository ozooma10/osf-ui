// OSF UI settings service. Header-only; link nothing.
// Calls are thread-safe; getters are synchronous. Callbacks run on the main thread.
// Callback strings are valid only for the call.
#pragma once

#include <cstdint>
#include "REX/W32/KERNEL32.h"

static_assert(sizeof(void *) == 8, "OSFUI_Settings requires x64");

namespace OSFUI::API::Settings
{
	// Packed major.minor service versions.
	inline constexpr std::uint32_t kVersion = 0x00010000u;
	inline constexpr std::uint32_t kBaseVersion = 0x00010000u;
	inline constexpr const char *kRequestExportName = "OSFUI_RequestSettings";

	// True when both versions share a major and a_have meets a_need.
	constexpr bool Supports(std::uint32_t a_have, std::uint32_t a_need) noexcept
	{
		return (a_have >> 16) == (a_need >> 16) && (a_have & 0xFFFFu) >= (a_need & 0xFFFFu);
	}

	// Receives (modId, key, valueJson, user).
	using SettingChangedFn = void (*)(const char* a_mod, const char* a_key, const char* a_valueJson, void *a_user) noexcept;
	// Receives (modId, key, user) when the bound physical key is pressed.
	using HotkeyFn = void (*)(const char* a_mod, const char* a_key, void *a_user) noexcept;

	struct ISettings
	{
		// Subscribes to all settings for a mod and replays current values. Returns 0 on failure.
		virtual std::uint32_t SubscribeSettings(const char* a_mod, SettingChangedFn a_fn, void *a_user) = 0;
		// Removes a settings subscription; unknown tokens are ignored.
		virtual void UnsubscribeSettings(std::uint32_t) = 0;
		// Reads a bool setting. Returns false for missing keys or type mismatch.
		virtual bool GetSettingBool(const char* a_mod, const char* a_key, bool* a_out) = 0;
		// Reads an integer setting. Returns false for missing keys or type mismatch.
		virtual bool GetSettingInt(const char* a_mod, const char* a_key, std::int64_t *a_out) = 0;
		// Reads a floating-point setting. Returns false for missing keys or type mismatch.
		virtual bool GetSettingFloat(const char* a_mod, const char* a_key, double *a_out) = 0;
		// Reads string, enum, or key text. Returns required bytes including NUL; 0 on failure.
		virtual std::uint32_t GetSettingString(const char* a_mod, const char* a_key, char *a_out, std::uint32_t a_outSize) = 0;
		// Subscribes to a key setting's current binding. Returns 0 on failure.
		virtual std::uint32_t SubscribeHotkey(const char* a_mod, const char* a_key, HotkeyFn a_fn, void *a_user) = 0;
		// Removes a hotkey subscription; unknown tokens are ignored.
		virtual void UnsubscribeHotkey(std::uint32_t) = 0;

	protected:
		// OSF UI owns the interface.
		~ISettings() = default;
	};

	using AcquireFn = void *(*)(std::uint32_t, std::uint32_t *) noexcept;

	// Acquires the service; outVersion receives the host version or 0 on failure.
	inline ISettings *RequestInterface(std::uint32_t a_version = kBaseVersion, std::uint32_t *a_outVersion = nullptr) noexcept
	{
		if (a_outVersion) {
			*a_outVersion = 0;
		}
		const auto module = REX::W32::GetModuleHandleW(L"OSFUI.dll");
		if (!module) {
			return nullptr;
		}
		const auto fn = reinterpret_cast<AcquireFn>(REX::W32::GetProcAddress(module, kRequestExportName));
		return fn ? static_cast<ISettings *>(fn(a_version, a_outVersion)) : nullptr;
	}

	class Client
	{
	public:
		// Acquires and caches the service. Call once after SFSE post-load.
		bool Init(std::uint32_t a_version = kBaseVersion) noexcept
		{
			std::uint32_t actual = 0;
			return Attach(RequestInterface(a_version, &actual), actual);
		}
		// Attaches a fetched interface or detaches on nullptr/incompatible version.
		bool Attach(ISettings *am_api, std::uint32_t a_version = kVersion) noexcept
		{
			m_api = am_api && Supports(a_version, kBaseVersion) ? am_api : nullptr;
			m_version = m_api ? a_version : 0;
			return m_api != nullptr;
		}

		// True when attached.
		[[nodiscard]] explicit operator bool() const noexcept { return m_api != nullptr; }
		// Returns the host service version, or 0 when detached.
		[[nodiscard]] std::uint32_t Version() const noexcept { return m_version; }
		// Tests support for a service version.
		[[nodiscard]] bool Has(std::uint32_t a_version) const noexcept { return m_api && Supports(m_version, a_version); }
		[[nodiscard]] ISettings *Raw() const noexcept { return m_api; }

		std::uint32_t SubscribeSettings(const char *a_mod, SettingChangedFn a_fn, void *a_user) const noexcept
		{
			return m_api ? m_api->SubscribeSettings(a_mod, a_fn, a_user) : 0;
		}
		void UnsubscribeSettings(std::uint32_t a_token) const noexcept
		{
			if (m_api) {
				m_api->UnsubscribeSettings(a_token);
			}
		}
		bool GetSettingBool(const char *a_mod, const char *a_key, bool *a_out) const noexcept
		{
			return m_api && m_api->GetSettingBool(a_mod, a_key, a_out);
		}
		bool GetSettingInt(const char *a_mod, const char *a_key, std::int64_t *a_out) const noexcept
		{
			return m_api && m_api->GetSettingInt(a_mod, a_key, a_out);
		}
		bool GetSettingFloat(const char *a_mod, const char *a_key, double *a_out) const noexcept
		{
			return m_api && m_api->GetSettingFloat(a_mod, a_key, a_out);
		}
		std::uint32_t GetSettingString(const char *a_mod, const char *a_key, char *a_buf, std::uint32_t a_len) const noexcept
		{
			return m_api ? m_api->GetSettingString(a_mod, a_key, a_buf, a_len) : 0;
		}
		std::uint32_t SubscribeHotkey(const char *a_mod, const char *a_key, HotkeyFn a_fn, void *a_user) const noexcept
		{
			return m_api ? m_api->SubscribeHotkey(a_mod, a_key, a_fn, a_user) : 0;
		}
		void UnsubscribeHotkey(std::uint32_t a_token) const noexcept
		{
			if (m_api) {
				m_api->UnsubscribeHotkey(a_token);
			}
		}

	private:
		ISettings *m_api{nullptr};
		std::uint32_t m_version{0};
	};
}
