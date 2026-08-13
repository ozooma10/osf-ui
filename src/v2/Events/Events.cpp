#include "Events.h"

#include "Handlers/MenuOpenClose.h"

namespace Events
{
    bool Register(MenuOpenCloseCallback a_callback)
    {
        if (!a_callback) {
            REX::ERROR("Events: menu event callback is unavailable");
            return false;
        }

        auto* const handler = Handlers::MenuOpenCloseHandler::GetSingleton();
        handler->SetCallback(a_callback);

        static bool registered = false;

        if (registered) {
            return true;
        }
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            REX::ERROR("Events: RE::UI singleton unavailable; menu events were not registered");
            return false;
        }

        ui->RegisterSink<RE::MenuOpenCloseEvent>(handler);

        registered = true;
        REX::INFO("Events: registered MenuOpenCloseEvent handler");
        return true;
    }
}
