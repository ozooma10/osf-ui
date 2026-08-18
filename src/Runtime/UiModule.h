#pragma once

namespace OSFUI
{
	class MessageBridge;

	class IUiModule
	{
	public:
		virtual ~IUiModule() = default;

		virtual void OnStart() {}
		virtual void RegisterEndpoints(MessageBridge& a_bridge) = 0;
		virtual void OnBridgeDown() {}
		virtual void OnViewDestroyed(std::string_view /*a_viewId*/) {}

		[[nodiscard]] virtual std::string_view Name() const = 0;
	};
}
