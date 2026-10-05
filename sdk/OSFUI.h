// OSF UI native service.
#pragma once

#include <cstdint>
#include <type_traits>
#include "REX/W32/KERNEL32.h"

static_assert(sizeof(void*) == 8, "OSFUI requires x64");

namespace OSFUI::API
{
	// Packed major.minor ABI version, independent of the plugin release version. Additions bump minor;
	// breaking changes need a new major. API 1.x is rejected; 1.6 consumers use OSFUI_RequestBridge (ABI 1.7).
	inline constexpr std::uint32_t kVersion = 0x00020000u;
	inline constexpr std::uint32_t kBaseVersion = 0x00020000u;
	inline constexpr wchar_t kModuleName[] = L"OSFUI.dll";
	inline constexpr char kRequestExportName[] = "OSFUI_RequestAPI";

	constexpr bool Supports(std::uint32_t have, std::uint32_t need) noexcept
	{
		return (have >> 16) == (need >> 16) && (have & 0xFFFFu) >= (need & 0xFFFFu);
	}

	using SendFn = void (*)(const char* name, const char* payloadJson, const char* sourceViewId, void* context) noexcept;
	// Receives accumulated relative-pointer motion for an exact view.
	enum class RelativePointerPhase : std::uint32_t
	{
		kBegin,   // Capture started.
		kUpdate,  // Per-frame accumulated dx, dy, and wheel.
		kEnd,     // Primary button released.
		kCancel,  // Capture or view ownership was lost.
	};
	using RelativePointerFn = void (*)(const char* viewId, RelativePointerPhase phase, float dx, float dy, float wheel, void* context) noexcept;
	// Menu presentation for an exact view.
	enum class ViewLifecyclePhase : std::uint32_t
	{
		kShown,   // Menu became logically presented.
		kFrame,   // One runtime update while shown; not a rendered-frame count.
		kHidden,  // Menu stopped being presented.
	};
	using ViewLifecycleFn = void (*)(const char* viewId, ViewLifecyclePhase phase, void* context) noexcept;

	// Copyable deferred reply token. Copies may answer later from any thread; only the first answer counts.
	struct Request
	{
		using ReplyFn = void (*)(std::uint64_t token, const char* json) noexcept;
		using RejectFn = void (*)(std::uint64_t token, const char* code, const char* message) noexcept;

		const char* name{};          // Registered endpoint; valid only for the callback.
		const char* payloadJson{};   // Caller payload; valid only for the callback.
		const char* sourceViewId{};  // Sending view; valid only for the callback.

		// Resolves with a JSON payload. Invalid JSON rejects with "invalid-response".
		void Reply(const char* json) const noexcept
		{
			if (m_reply) m_reply(m_token, json);
		}
		// Rejects with a stable code and optional message.
		void Reject(const char* code, const char* message = "") const noexcept
		{
			if (m_reject) m_reject(m_token, code, message);
		}

		// Host-owned reply state; copy it, do not modify it.
		std::uint64_t m_token{};
		ReplyFn m_reply{};
		RejectFn m_reject{};
	};
	static_assert(std::is_standard_layout_v<Request> && std::is_trivially_copyable_v<Request>);

	using RequestFn = void (*)(const Request& request, void* context) noexcept;

	struct IUI
	{
		// True while any web document is live (stays true across a reload). Other slots may be called before readiness.
		virtual bool IsReady() noexcept = 0;

		// Register once at load. Names are exact and unique across sends, requests and Papyrus; false for null,
		// invalid, reserved ("osfui.") or taken names. Page names resolve in the owning mod namespace first.
		virtual bool RegisterSend(const char* name, SendFn callback, void* context) noexcept = 0;
		virtual bool RegisterRequest(const char* name, RequestFn callback, void* context) noexcept = 0;

		// Transient event for page on(event); oldest per-view events drop under pressure. false for invalid view,
		// empty or >128-byte event, invalid JSON, or payload over 1 MiB.
		virtual bool EmitEvent(const char* viewId, const char* event, const char* payloadJson) noexcept = 0;
		// Mod-scoped retained state, replayed to fresh documents. false for invalid mod, empty/overlong key,
		// invalid JSON, or a full pending queue.
		virtual bool SetState(const char* modId, const char* key, const char* valueJson) noexcept = 0;

		// viewId: "mod/view", case-insensitive. false for an unknown view; close also cancels a pending open.
		virtual bool RequestMenu(const char* viewId, bool open) noexcept = 0;
		// Loads a shipped view folder; idempotent. false for an invalid view or mod ID.
		virtual bool RegisterView(const char* viewId) noexcept = 0;

		// First registration per view wins; false for invalid ID, null callback, or existing owner.
		// Unregister accepts unknown views and cancels any active capture.
		virtual bool RegisterRelativePointer(const char* viewId, RelativePointerFn callback, void* context) noexcept = 0;
		virtual void UnregisterRelativePointer(const char* viewId) noexcept = 0;
		virtual bool RegisterViewLifecycle(const char* viewId, ViewLifecycleFn callback, void* context) noexcept = 0;
		virtual void UnregisterViewLifecycle(const char* viewId) noexcept = 0;

	protected:
		~IUI() = default;
	};

	// Borrowed process-lifetime interface, or nullptr if unsupported. outVersion (optional): actual ABI, or 0 on failure.
	using AcquireFn = void* (*)(std::uint32_t version, std::uint32_t* outVersion) noexcept;

	inline IUI* RequestInterface(std::uint32_t version = kBaseVersion, std::uint32_t* outVersion = nullptr) noexcept
	{
		if (outVersion) *outVersion = 0;
		if (!Supports(version, kBaseVersion)) return nullptr;
		const auto module = REX::W32::GetModuleHandleW(kModuleName);
		if (!module) return nullptr;
		const auto fn = reinterpret_cast<AcquireFn>(REX::W32::GetProcAddress(module, kRequestExportName));
		if (!fn) return nullptr;
		std::uint32_t actual{};
		auto* api = fn(version, &actual);
		if (!api || !Supports(actual, version)) return nullptr;
		if (outVersion) *outVersion = actual;
		return static_cast<IUI*>(api);
	}

	class Client
	{
	public:
		// Acquires and caches the service after SFSE kPostPostLoad.
		bool Init(std::uint32_t version = kBaseVersion) noexcept
		{
			std::uint32_t actual{};
			auto* api = RequestInterface(version, &actual);
			return Attach(api, actual);
		}

		// Borrows the interface; nullptr or an incompatible version detaches.
		bool Attach(IUI* api, std::uint32_t version = kVersion) noexcept
		{
			m_api = api && Supports(version, kBaseVersion) ? api : nullptr;
			m_version = m_api ? version : 0;
			return m_api != nullptr;
		}

		[[nodiscard]] explicit operator bool() const noexcept { return m_api != nullptr; }
		[[nodiscard]] std::uint32_t Version() const noexcept { return m_version; }
		[[nodiscard]] bool Has(std::uint32_t version) const noexcept { return m_api && Supports(m_version, version); }
		[[nodiscard]] IUI* Raw() const noexcept { return m_api; }
		[[nodiscard]] bool IsReady() const noexcept { return m_api && m_api->IsReady(); }

		bool RegisterSend(const char* name, SendFn callback, void* context) const noexcept
		{
			return m_api && m_api->RegisterSend(name, callback, context);
		}
		bool RegisterRequest(const char* name, RequestFn callback, void* context) const noexcept
		{
			return m_api && m_api->RegisterRequest(name, callback, context);
		}

		bool EmitEvent(const char* viewId, const char* event, const char* payloadJson) const noexcept
		{
			return m_api && m_api->EmitEvent(viewId, event, payloadJson);
		}
		bool SetState(const char* modId, const char* key, const char* valueJson) const noexcept
		{
			return m_api && m_api->SetState(modId, key, valueJson);
		}

		bool RequestMenu(const char* viewId, bool open) const noexcept
		{
			return m_api && m_api->RequestMenu(viewId, open);
		}
		bool RegisterView(const char* viewId) const noexcept
		{
			return m_api && m_api->RegisterView(viewId);
		}

		bool RegisterRelativePointer(const char* viewId, RelativePointerFn callback, void* context) const noexcept
		{
			return m_api && m_api->RegisterRelativePointer(viewId, callback, context);
		}
		void UnregisterRelativePointer(const char* viewId) const noexcept
		{
			if (m_api) m_api->UnregisterRelativePointer(viewId);
		}
		bool RegisterViewLifecycle(const char* viewId, ViewLifecycleFn callback, void* context) const noexcept
		{
			return m_api && m_api->RegisterViewLifecycle(viewId, callback, context);
		}
		void UnregisterViewLifecycle(const char* viewId) const noexcept
		{
			if (m_api) m_api->UnregisterViewLifecycle(viewId);
		}

	private:
		IUI* m_api{};
		std::uint32_t m_version{};
	};
}
