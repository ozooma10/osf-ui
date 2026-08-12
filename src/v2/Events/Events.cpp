#include "Events.h"

#include "Handlers/MenuOpenClose.h"

namespace Events
{
    bool Register()
    {
        static bool registered = false;

        if(registered) {
            return true;
        }
        auto* ui = RE::UI::GetSingleton();
        if (!ui) {
            REX::ERROR("Events: RE::UI singleton unavailable; menu events were not registered");
            return false;
        }

        ui->RegisterSink<RE::MenuOpenCloseEvent>(Handlers::MenuOpenCloseHandler::GetSingleton());

        registered = true;
        REX::INFO("Events: registered MenuOpenCloseEvent handler");
        return true;
    }
}