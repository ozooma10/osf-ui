#pragma once

#include "OSFUI_API.h"  // the public ABI (IOSFUIBridge, CommandFn, ReadyFn)

#include <nlohmann/json.hpp>

// Internal implementation of the public OSF UI native bridge API.
//
// BridgeApi keeps its OWN command registry independent of MessageBridge lifetime. 
// A consumer can RegisterCommand before any bridge exists (before/after its DOM is ready, before/after our Runtime::Initialize). 
// The registration is (re)applied to the live MessageBridge on the game main thread and re-applied verbatim if the bridge is torn down and rebuilt, the consumer never re-registers.

namespace OSFUI
{
	class MessageBridge;
}

namespace OSFUI::API
{
	class BridgeApi final : public IOSFUIBridge
	{
	public:
		[[nodiscard]] static BridgeApi& Get();


		std::uint32_t GetInterfaceVersion() override;
		void          GetPluginVersion(std::uint32_t& a_major, std::uint32_t& a_minor, std::uint32_t& a_patch) override;
		const char*   GetBridgeProtocolVersion() override;
		bool          IsBridgeReady() override;
		void          RegisterCommand(const char* a_command, CommandFn a_handler, void* a_user) override;
		void          UnregisterCommand(const char* a_command) override;
		bool          SendToWeb(const char* a_viewId, const char* a_type, const char* a_payloadJson) override;
		void          SetReadyCallback(ReadyFn a_callback, void* a_user) override;


		void OnBridgeReady(MessageBridge* a_bridge);
		void PumpMainThread();

	private:
		BridgeApi() = default;

		struct Registration
		{
			CommandFn fn{ nullptr };
			void*     user{ nullptr };
		};

		struct Op
		{
			enum class Kind
			{
				Register,
				Unregister
			};

			Kind         kind;
			std::string  command;
			Registration reg;  // valid for Register
		};

		// A queued off-thread native->web message (payload pre-parsed so the caller learns about malformed JSON synchronously).
		struct PendingSend
		{
			std::string    view;
			std::string    type;
			nlohmann::json payload;
		};

		// (main thread) register the ABI-safe CommandFn on the live bridge via a trampoline that adapts nlohmann::json <-> JSON text.
		void ApplyCommand(const std::string& a_command, const Registration& a_reg);

		static bool IsReservedPrefix(std::string_view a_command);

		std::mutex _mutex;  // guards everything below except _bridge/_ready

		std::unordered_map<std::string, Registration> _commands;  // net desired state (for full re-apply)
		std::vector<Op>                               _ops;       // deltas to apply on next pump
		std::vector<PendingSend>                      _pendingSends;
		ReadyFn                                       _readyCb{ nullptr };
		void*                                         _readyUser{ nullptr };
		bool                                          _firePendingReady{ false };

		// Non-owning; assigned/dereferenced on the main thread only.
		MessageBridge*    _bridge{ nullptr };
		std::atomic_bool  _ready{ false };  // a nativeBridge view is live (lock-free status read)
	};
}
