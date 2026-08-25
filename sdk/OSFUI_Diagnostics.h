// OSF UI diagnostics service. Header-only; link nothing.
// Report durable, actionable conditions; use normal logging for routine events.
// Calls are thread-safe; accepted changes land on the game main thread.
#pragma once

#include <cstdint>
#include "REX/W32/KERNEL32.h"


static_assert(sizeof(void*) == 8, "OSFUI_Diagnostics requires x64");

namespace OSFUI::API::Diagnostics
{
	// Packed major.minor service versions.
	inline constexpr std::uint32_t kVersion = 0x00010000u;
	inline constexpr std::uint32_t kBaseVersion = 0x00010000u;
	inline constexpr const char* kRequestExportName = "OSFUI_RequestDiagnostics";

	// True when both versions share a major and a_have meets a_need.
	constexpr bool Supports(std::uint32_t a_have, std::uint32_t a_need) noexcept
	{
		return (a_have >> 16) == (a_need >> 16) && (a_have & 0xFFFFu) >= (a_need & 0xFFFFu);
	}

	enum class IssueSeverity : std::uint32_t
	{
		kWarning,  // Degraded but usable.
		kError,    // The affected feature does not work.
	};

	struct IDiagnostics
	{
		// Raises or updates modId/id. contextJson is an optional flat JSON object.
		virtual bool ReportIssue(const char* a_mod, const char* a_id, const char* a_code, std::uint32_t a_severity, const char* a_subject, const char* a_context) = 0;
		// Resolves one issue. True means accepted, even if the id was not active.
		virtual bool ClearIssue(const char* a_mod, const char* a_id) = 0;
		// Resolves all active issues for a mod except ids in a JSON string array.
		virtual bool ClearIssuesExcept(const char* a_mod, const char* a_idsJson) = 0;

	protected:
		// OSF UI owns the interface.
		~IDiagnostics() = default;
	};

	using AcquireFn = void* (*)(std::uint32_t, std::uint32_t*) noexcept;

	// Acquires the service; outVersion receives the host version or 0 on failure.
	inline IDiagnostics* RequestInterface(std::uint32_t a_version = kBaseVersion, std::uint32_t* a_outVersion = nullptr) noexcept
	{
		if (a_outVersion) *a_outVersion = 0;
		const auto module = REX::W32::GetModuleHandleW(L"OSFUI.dll");
		if (!module) return nullptr;
		const auto fn = reinterpret_cast<AcquireFn>(REX::W32::GetProcAddress(module, kRequestExportName));
		return fn ? static_cast<IDiagnostics*>(fn(a_version, a_outVersion)) : nullptr;
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
		bool Attach(IDiagnostics* a_api, std::uint32_t a_version = kVersion) noexcept
		{
			m_api = a_api && Supports(a_version, kBaseVersion) ? a_api : nullptr;
			m_version = m_api ? a_version : 0;
			return m_api != nullptr;
		}

		// True when attached.
		[[nodiscard]] explicit operator bool() const noexcept { return m_api != nullptr; }
		// Returns the host service version, or 0 when detached.
		[[nodiscard]] std::uint32_t Version() const noexcept { return m_version; }
		// Tests support for a service version.
		[[nodiscard]] bool Has(std::uint32_t am_version) const noexcept { return m_api && Supports(m_version, am_version); }
		// Returns the host-owned interface for advanced use.
		[[nodiscard]] IDiagnostics* Raw() const noexcept { return m_api; }

		bool ReportIssue(const char* a_mod, const char* a_id, const char* a_code, IssueSeverity a_severity, const char* a_subject = "", const char* a_context = nullptr) const noexcept
		{
			return m_api && m_api->ReportIssue(a_mod, a_id, a_code, static_cast<std::uint32_t>(a_severity), a_subject, a_context);
		}
		bool ClearIssue(const char* a_mod, const char* a_id) const noexcept
		{
			return m_api && m_api->ClearIssue(a_mod, a_id);
		}
		bool ClearIssuesExcept(const char* a_mod, const char* a_keep) const noexcept
		{
			return m_api && m_api->ClearIssuesExcept(a_mod, a_keep);
		}

	private:
		IDiagnostics* m_api{ nullptr };
		std::uint32_t m_version{ 0 };
	};
}
