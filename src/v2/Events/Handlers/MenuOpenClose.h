#pragma once

#include "../Events.h"

#include <atomic>

namespace Events::Handlers
{
    class MenuOpenCloseHandler : public REX::TSingleton<MenuOpenCloseHandler>, public RE::BSTEventSink<RE::MenuOpenCloseEvent>
    {
    public:
        void SetCallback(MenuOpenCloseCallback a_callback)
        {
            _callback.store(a_callback, std::memory_order_release);
        }

        virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent& a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
        {
            REX::INFO("MenuOpenCloseEvent: menuName = {}, opening = {}", a_event.menuName.c_str(), a_event.opening);

            if (const auto callback = _callback.load(std::memory_order_acquire)) {
                callback(std::string_view {a_event.menuName}, a_event.opening);
            }

            return RE::BSEventNotifyControl::kContinue;
        }

    private:
        std::atomic<MenuOpenCloseCallback> _callback {nullptr};
    };
}
