#pragma once

namespace Events::Handlers
{
    class MenuOpenCloseHandler : public REX::TSingleton<MenuOpenCloseHandler>, public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
            REX::INFO("MenuOpenCloseEvent: menuName = {}, opening = {}", a_event.menuName.c_str(), a_event.opening);
			return RE::BSEventNotifyControl::kContinue;
		}
    };
}