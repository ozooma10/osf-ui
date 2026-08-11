#pragma once

#include "Handlers/MenuOpenClose.h"

namespace Events
{
    void Register()
    {
        REX::INFO("Registering event handlers...");
        if(auto UI = RE::UI::GetSingleton())
        {
            REX::INFO("Registering MenuOpenCloseHandler...");
            UI->RegisterSink<RE::MenuOpenCloseEvent>(Handlers::MenuOpenCloseHandler::GetSingleton());
        }
    }
}