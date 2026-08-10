#pragma once

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

#include "compat/v1/NativeBridge.h"  // the frozen CommandFn / Request / RequestFn ABI shapes
#include "runtime/MessageBridge.h"

namespace OSFUI::Compat::V1
{
	// The temporary home of every 1.x endpoint the strict core no longer
	// models: legacy commands (one name callable as send AND request, with
	// request-id injection and auto-ack) and legacy requests (typed replies,
	// wrapped in the `__osfuiV1Reply` envelope for 1.x documents).
	//
	// Ownership boundary: BridgeApi only reserves endpoint names (first-wins
	// across strict and legacy, no nested locks), and MessageBridge only
	// consults the IV1DispatchShim at its unknown-endpoint boundary. The maps,
	// the dispatch semantics, and the typed-reply ledger with its deadline all
	// live here — removing 1.x support is deleting this class and unhooking
	// the two seams.
	//
	// Threading mirrors BridgeApi: registration and the reply thunks are
	// any-thread; dispatch and Pump run on the main thread. Handlers are
	// copied out under the lock and invoked outside it, so a same-tick
	// unregister can never invalidate an in-progress call.
	class LegacyEndpoints final : public MessageBridge::IV1DispatchShim
	{
	public:
		[[nodiscard]] static LegacyEndpoints& Get();

		// ABI entry points (any thread), reached only through NativeBridge.
		void RegisterCommand(const char* a_name, CommandFn a_fn, void* a_user);
		void UnregisterCommand(const char* a_name);
		void RegisterRequest(const char* a_name, RequestFn a_fn, void* a_user);
		void UnregisterRequest(const char* a_name);

		// MessageBridge::IV1DispatchShim (main thread, inside dispatch).
		SendOutcome TryDispatchSend(const std::string& a_name,
			const nlohmann::json& a_payload, MessageBridge& a_bridge) override;
		bool TryDispatchRequest(const std::string& a_name, const std::string& a_id,
			const nlohmann::json& a_payload, MessageBridge& a_bridge) override;

		// Main thread, once per tick: flush queued 1.x request answers into the
		// bridge and expire ledger entries whose plugin never answered
		// (`no-response`, mirroring the strict path). A bridge instance change
		// clears the ledger first — defer tokens are minted per bridge
		// instance and must never settle a successor's request.
		void Pump(MessageBridge& a_bridge);

	private:
		struct Command
		{
			CommandFn fn{ nullptr };
			void*     user{ nullptr };
		};
		struct RequestReg
		{
			RequestFn fn{ nullptr };
			void*     user{ nullptr };
		};
		struct Inflight
		{
			std::string view;
			std::string deferToken;  // MessageBridge::Defer()'s token
			std::string name;
			bool        wrapReply{ false };  // the source was a 1.x document at dispatch time
			std::chrono::steady_clock::time_point deadline;
			bool        answered{ false };
			bool        rejected{ false };
			std::string type;  // 1.x typed reply; defaults to the endpoint name
			std::string payloadJson;
			std::string code;
			std::string message;
		};

		static void RespondThunk(std::uint64_t, const char*, const char*) noexcept;
		static void RejectThunk(std::uint64_t, const char*, const char*) noexcept;
		void RespondRequest(std::uint64_t a_token, const char* a_type, const char* a_json) noexcept;
		void RejectRequest(std::uint64_t a_token, const char* a_code, const char* a_message) noexcept;
		void DispatchRequest(const std::string& a_name, const RequestReg& a_reg,
			const nlohmann::json& a_payload, MessageBridge& a_bridge);

		std::mutex                                  _mutex;
		std::unordered_map<std::string, Command>    _commands;
		std::unordered_map<std::string, RequestReg> _requests;
		std::unordered_map<std::uint64_t, Inflight> _inflight;
		std::uint64_t                               _nextToken{ 1 };
		// The bridge instance whose defer tokens the ledger holds. Updated at
		// both dispatch and Pump (main thread); an instance change clears the
		// ledger so a stale token can never settle a successor's request.
		MessageBridge*                              _ledgerBridge{ nullptr };
	};
}
