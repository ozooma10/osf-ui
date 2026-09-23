// OSF UI native API. Header-only; link nothing.
// Mutations are thread-safe; callbacks run on the game main thread.
// Callback strings are valid only for the call.
#pragma once

#include <cstdint>
#include <type_traits>
#include "REX/W32/KERNEL32.h"


static_assert(sizeof(void*) == 8, "OSFUI requires x64");

namespace OSFUI::API
{
	// Packed major.minor API version. This export exposes the complete interface.
	inline constexpr std::uint32_t kVersion = 0x00010000u;
	inline constexpr const char* kRequestExportName = "OSFUI_RequestAPI";

	// Receives (name, payloadJson, sourceViewId, user) for a one-way send.
	using SendFn = void (*)(const char* a_name, const char* a_payloadJson, const char* a_sourceViewId, void* a_user) noexcept;
	// Runs when a bridge-enabled view becomes available or is recreated.
	using ReadyFn = void (*)(void* a_user) noexcept;

	// Copyable deferred reply token. Copies may answer later from any thread.
	struct Request
	{
		using RespondFn = void (*)(std::uint64_t, const char*) noexcept;
		using RejectFn = void (*)(std::uint64_t, const char*, const char*) noexcept;

		const char* name{ nullptr };          // Registered endpoint; callback lifetime.
		const char* payloadJson{ nullptr };   // Caller payload; callback lifetime.
		const char* sourceViewId{ nullptr };  // Sending view; callback lifetime.

		// Resolves the request with a JSON payload.
		void Respond(const char* a_json) const noexcept
		{
			if (_respond) _respond(_token, a_json);
		}
		// Rejects the request with a stable code and optional message.
		void Reject(const char* a_code, const char* a_message = "") const noexcept
		{
			if (_reject) _reject(_token, a_code, a_message);
		}

		// Host-owned reply state; copy it but do not modify it.
		std::uint64_t _token{ 0 };
		RespondFn _respond{ nullptr };
		RejectFn _reject{ nullptr };
	};

	static_assert(std::is_standard_layout_v<Request> && std::is_trivially_copyable_v<Request>);
	// Receives a request and the user pointer passed to RegisterRequest.
	using RequestFn = void (*)(const Request& a_request, void* a_user) noexcept;

	enum class RelativePointerPhase : std::uint32_t
	{
		kBegin,   // Capture started.
		kUpdate,  // Per-frame accumulated dx, dy, and wheel.
		kEnd,     // Primary button released.
		kCancel,  // Capture or view ownership was lost.
	};

	// Receives (viewId, phase, dx, dy, wheel, user).
	using RelativePointerFn = void (*)(const char* a_viewId, RelativePointerPhase a_phase, float a_dx, float a_dy, float a_wheel, void* a_user) noexcept;
	// Receives (viewId, user); false blocks the pending open.
	using ViewOpenPreflightFn = bool (*)(const char* a_viewId, void* a_user) noexcept;

	enum class ViewLifecyclePhase : std::uint32_t
	{
		kShown,   // Menu became logically presented.
		kFrame,   // One game-main-thread tick while shown.
		kHidden,  // Menu stopped being presented.
	};

	// Receives (viewId, phase, user) for an exact menu view.
	using ViewLifecycleFn = void (*)(const char* a_viewId, ViewLifecyclePhase a_phase, void* a_user) noexcept;

	using InteractionToken = std::uint64_t;
	enum class InteractionPhase : std::uint32_t { kStarted, kEnded, kRejected };
	// Main thread; reason and viewId have callback lifetime. Keep context alive until Ended/Rejected.
	using InteractionFn = void (*)(InteractionToken a_token, const char* a_viewId,
		InteractionPhase a_phase, const char* a_reason, void* a_context) noexcept;

	struct IUI
	{
		// True while at least one bridge-enabled document is live.
		virtual bool IsReady() = 0;
		// Registers a one-way endpoint; callbacks run on the main thread.
		virtual void RegisterSend(const char* a_name, SendFn a_callback, void* a_user) = 0;
		// Removes a one-way endpoint; unknown names are ignored.
		virtual void UnregisterSend(const char* a_name) = 0;
		// Registers a request/reply endpoint; callbacks run on the main thread.
		virtual void RegisterRequest(const char* a_name, RequestFn a_callback, void* a_user) = 0;
		// Removes a request endpoint; unknown names are ignored.
		virtual void UnregisterRequest(const char* a_name) = 0;
		// Queues a one-shot event for a view. payload must be valid JSON.
		virtual bool SendToWeb(const char* a_viewId, const char* a_event, const char* a_payload) = 0;
		// Stores mod-scoped state and replays it to fresh documents.
		virtual bool SetViewState(const char* a_modId, const char* a_key, const char* a_value) = 0;
		// Sets the bridge-availability callback; replaces the previous callback.
		virtual void SetReadyCallback(ReadyFn a_callback, void* a_user) = 0;
		// Opens or closes a qualified modId/viewName surface.
		virtual bool RequestMenu(const char* a_viewId, bool a_open) = 0;
		// Loads and registers a shipped view folder. Repeated calls are safe.
		virtual bool RegisterView(const char* a_viewId) = 0;
		// Registers raw relative-pointer delivery for one exact view.
		virtual bool RegisterRelativePointer(const char* a_viewId, RelativePointerFn a_callback, void* a_user) = 0;
		// Removes relative-pointer delivery and cancels active capture.
		virtual void UnregisterRelativePointer(const char* a_viewId) = 0;
		// Registers a synchronous allow/deny callback before a view opens.
		virtual bool RegisterViewOpenPreflight(const char* a_viewId, ViewOpenPreflightFn a_callback, void* a_user) = 0;
		// Removes a view-open preflight callback.
		virtual void UnregisterViewOpenPreflight(const char* a_viewId) = 0;
		// Registers shown/frame/hidden callbacks for one exact menu view.
		virtual bool RegisterViewLifecycle(const char* a_viewId, ViewLifecycleFn a_callback, void* a_user) = 0;
		// Removes lifecycle callbacks for a view.
		virtual void UnregisterViewLifecycle(const char* a_viewId) = 0;
		// Queue exclusive keyboard/gamepad ownership of an already loaded world view.
		// Zero means not queued. Otherwise exactly one Started then Ended, or one Rejected.
		// Admission waits for released input and expires after five seconds.
		// Escape/Back, focus return, a menu opening, or browser failure ends ownership.
		virtual InteractionToken BeginInteraction(const char* a_viewId, InteractionFn a_callback, void* a_context) = 0;
		// Ends only this token; stale tokens cannot release a newer owner.
		virtual bool EndInteraction(InteractionToken a_token) = 0;

	protected:
		// OSF UI owns the interface.
		~IUI() = default;
	};

	using AcquireFn = void* (*)(std::uint32_t, std::uint32_t*) noexcept;

	// Acquires the service; outVersion receives the host version or 0 on failure.
	inline IUI* RequestInterface(std::uint32_t a_version = kVersion, std::uint32_t* a_outVersion = nullptr) noexcept
	{
		if (a_outVersion) *a_outVersion = 0;
		const auto module = REX::W32::GetModuleHandleW(L"OSFUI.dll");
		if (!module) return nullptr;
		const auto fn = reinterpret_cast<AcquireFn>(REX::W32::GetProcAddress(module, kRequestExportName));
		return fn ? static_cast<IUI*>(fn(a_version, a_outVersion)) : nullptr;
	}

	class Client
	{
	public:
		// Acquires and caches the service. Call once after SFSE post-load.
		bool Init(std::uint32_t a_version = kVersion) noexcept
		{
			std::uint32_t actual = 0;
			auto* api = RequestInterface(a_version, &actual);
			return Attach(api, actual);
		}
		// Attaches a fetched interface or detaches on nullptr/incompatible version.
		bool Attach(IUI* a_api, std::uint32_t a_version = kVersion) noexcept
		{
			m_api = a_api && a_version == kVersion ? a_api : nullptr;
			m_version = m_api ? a_version : 0;
			return m_api != nullptr;
		}

		// True when attached.
		[[nodiscard]] explicit operator bool() const noexcept { return m_api != nullptr; }
		// Returns the host service version, or 0 when detached.
		[[nodiscard]] std::uint32_t Version() const noexcept { return m_version; }
		// Returns the host-owned interface for advanced use.
		[[nodiscard]] IUI* Raw() const noexcept { return m_api; }
		// True while at least one bridge-enabled document is live.
		[[nodiscard]] bool IsReady() const noexcept { return m_api && m_api->IsReady(); }

		void RegisterSend(const char* a_name, SendFn a_fn, void* a_user) const noexcept
		{
			if (m_api) m_api->RegisterSend(a_name, a_fn, a_user);
		}
		void UnregisterSend(const char* a_name) const noexcept
		{
			if (m_api) m_api->UnregisterSend(a_name);
		}
		void RegisterRequest(const char* a_name, RequestFn a_fn, void* a_user) const noexcept
		{
			if (m_api) m_api->RegisterRequest(a_name, a_fn, a_user);
		}
		void UnregisterRequest(const char* a_name) const noexcept
		{
			if (m_api) m_api->UnregisterRequest(a_name);
		}
		bool SendToWeb(const char* a_view, const char* a_type, const char* a_json) const noexcept
		{
			return m_api && m_api->SendToWeb(a_view, a_type, a_json);
		}
		bool SetViewState(const char* a_mod, const char* a_key, const char* a_json) const noexcept
		{
			return m_api && m_api->SetViewState(a_mod, a_key, a_json);
		}
		void SetReadyCallback(ReadyFn a_fn, void* a_user) const noexcept
		{
			if (m_api) m_api->SetReadyCallback(a_fn, a_user);
		}

		bool RequestMenu(const char* a_view, bool a_open) const noexcept
		{
			return m_api && m_api->RequestMenu(a_view, a_open);
		}
		bool RegisterView(const char* a_view) const noexcept
		{
			return m_api && m_api->RegisterView(a_view);
		}
		bool RegisterRelativePointer(const char* a_view, RelativePointerFn a_fn, void* a_user) const noexcept
		{
			return m_api && m_api->RegisterRelativePointer(a_view, a_fn, a_user);
		}
		void UnregisterRelativePointer(const char* a_view) const noexcept
		{
			if (m_api) m_api->UnregisterRelativePointer(a_view);
		}
		bool RegisterViewOpenPreflight(const char* a_view, ViewOpenPreflightFn a_fn, void* a_user) const noexcept
		{
			return m_api && m_api->RegisterViewOpenPreflight(a_view, a_fn, a_user);
		}
		void UnregisterViewOpenPreflight(const char* a_view) const noexcept
		{
			if (m_api) m_api->UnregisterViewOpenPreflight(a_view);
		}
		bool RegisterViewLifecycle(const char* a_view, ViewLifecycleFn a_fn, void* a_user) const noexcept
		{
			return m_api && m_api->RegisterViewLifecycle(a_view, a_fn, a_user);
		}
		void UnregisterViewLifecycle(const char* a_view) const noexcept
		{
			if (m_api) m_api->UnregisterViewLifecycle(a_view);
		}
		InteractionToken BeginInteraction(const char* a_view, InteractionFn a_callback, void* a_context) const noexcept
		{
			return m_api ? m_api->BeginInteraction(a_view, a_callback, a_context) : 0;
		}
		bool EndInteraction(InteractionToken a_token) const noexcept
		{
			return m_api && m_api->EndInteraction(a_token);
		}

	private:
		IUI* m_api{ nullptr };
		std::uint32_t m_version{ 0 };
	};
}
