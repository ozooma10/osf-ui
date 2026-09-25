// OSF UI native service.
#pragma once

#include <cstdint>
#include <type_traits>
#include "REX/W32/KERNEL32.h"

static_assert(sizeof(void*) == 8, "OSFUI requires x64");

namespace OSFUI::API
{
	// Packed major.minor ABI versions, independent of the plugin release version.
	inline constexpr std::uint32_t kVersion = 0x00010000u;
	inline constexpr std::uint32_t kBaseVersion = 0x00010000u;
	inline constexpr wchar_t kModuleName[] = L"OSFUI.dll";
	inline constexpr char kRequestExportName[] = "OSFUI_RequestAPI";

	constexpr bool Supports(std::uint32_t have, std::uint32_t need) noexcept
	{
		return (have >> 16) == (need >> 16) && (have & 0xFFFFu) >= (need & 0xFFFFu);
	}

	// All callbacks run on the game main thread.
	using SendFn = void (*)(const char* name, const char* payloadJson, const char* sourceViewId, void* context) noexcept;
	// Runs when a bridge-enabled view becomes available or is recreated.
	using ReadyFn = void (*)(void* context) noexcept;
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
		kFrame,   // One game-main-thread tick while shown.
		kHidden,  // Menu stopped being presented.
	};
	using ViewLifecycleFn = void (*)(const char* viewId, ViewLifecyclePhase phase, void* context) noexcept;

	// Copyable deferred reply token. Copies may answer later from any thread; only the first answer counts.
	struct Request
	{
		using RespondFn = void (*)(std::uint64_t token, const char* json) noexcept;
		using RejectFn = void (*)(std::uint64_t token, const char* code, const char* message) noexcept;

		const char* name{};          // Registered endpoint; valid only for the callback.
		const char* payloadJson{};   // Caller payload; valid only for the callback.
		const char* sourceViewId{};  // Sending view; valid only for the callback.

		// Resolves with a JSON payload. Invalid JSON rejects with "invalid-response".
		void Respond(const char* json) const noexcept
		{
			if (m_respond) m_respond(m_token, json);
		}
		// Rejects with a stable code and optional message.
		void Reject(const char* code, const char* message = "") const noexcept
		{
			if (m_reject) m_reject(m_token, code, message);
		}

		// Host-owned reply state; copy it, do not modify it.
		std::uint64_t m_token{};
		RespondFn m_respond{};
		RejectFn m_reject{};
	};
	static_assert(std::is_standard_layout_v<Request> && std::is_trivially_copyable_v<Request>);

	using RequestFn = void (*)(const Request& request, void* context) noexcept;

	struct IUI
	{
		// True while at least web document is live. Every other slot may be called before readiness.
		virtual bool IsReady() noexcept = 0;

		// Endpoint names are exact and unique across sends and requests; reserved prefixes such as "osfui." are refused.
		// Register once at load. false for a null, invalid, reserved, or already registered name (including Papyrus).
		// Local page names resolve in the owning mod namespace before global names, across both registries.
		virtual bool RegisterSend(const char* name, SendFn callback, void* context) noexcept = 0;
		virtual bool RegisterRequest(const char* name, RequestFn callback, void* context) noexcept = 0;

		// Queues a transient event for a view; the page receives it through on(event). payloadJson must be valid JSON.
		// false for an invalid view ID, event longer than 128 bytes, empty event, invalid JSON, or payload over 1 MiB.
		// Oldest queued events per view are dropped under pressure.
		virtual bool SendToWeb(const char* viewId, const char* event, const char* payloadJson) noexcept = 0;
		// Stores mod-scoped retained state and replays it to fresh documents. valueJson must be valid JSON.
		// false for an invalid mod ID, empty or overlong key, invalid JSON, or a full pending queue.
		virtual bool SetViewState(const char* modId, const char* key, const char* valueJson) noexcept = 0;
		// Replaces the previous callback. A callback installed while ready is invoked once on the next main-thread tick.
		virtual void SetReadyCallback(ReadyFn callback, void* context) noexcept = 0;

		// Views are qualified "mod/view" IDs, matched case-insensitively.
		// Queues an open or close for a discovered view; false when the view is unknown. Closing also cancels a queued/loading open. Repeated closes are harmless.
		virtual bool RequestMenu(const char* viewId, bool open) noexcept = 0;
		// Loads and registers a shipped view folder. Repeated calls are safe; false for an invalid view or mod ID.
		virtual bool RegisterView(const char* viewId) noexcept = 0;

		// One owner per exact view, first registration wins; false for an invalid ID, null callback, or an existing owner.
		// Unregister accepts unknown views and cancels any active relative-pointer capture.
		virtual bool RegisterRelativePointer(const char* viewId, RelativePointerFn callback, void* context) noexcept = 0;
		virtual void UnregisterRelativePointer(const char* viewId) noexcept = 0;
		virtual bool RegisterViewLifecycle(const char* viewId, ViewLifecycleFn callback, void* context) noexcept = 0;
		virtual void UnregisterViewLifecycle(const char* viewId) noexcept = 0;

	protected:
		~IUI() = default;
	};

	// Returns a borrowed process-lifetime interface, or nullptr for an unsupported major/minor. outVersion is optional: actual ABI on success, zero on failure.
	using AcquireFn = void* (*)(std::uint32_t version, std::uint32_t* outVersion) noexcept;

	inline IUI* RequestInterface(std::uint32_t version = kBaseVersion, std::uint32_t* outVersion = nullptr) noexcept
	{
		if (outVersion) *outVersion = 0;
		const auto module = REX::W32::GetModuleHandleW(kModuleName);
		if (!module) return nullptr;
		const auto fn = reinterpret_cast<AcquireFn>(REX::W32::GetProcAddress(module, kRequestExportName));
		return fn ? static_cast<IUI*>(fn(version, outVersion)) : nullptr;
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

		bool SendToWeb(const char* viewId, const char* event, const char* payloadJson) const noexcept
		{
			return m_api && m_api->SendToWeb(viewId, event, payloadJson);
		}
		bool SetViewState(const char* modId, const char* key, const char* valueJson) const noexcept
		{
			return m_api && m_api->SetViewState(modId, key, valueJson);
		}
		void SetReadyCallback(ReadyFn callback, void* context) const noexcept
		{
			if (m_api) m_api->SetReadyCallback(callback, context);
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
