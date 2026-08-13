#pragma once

#include "ViewManifest.h"

namespace Runtime
{
    class IViewPresenter
    {
    public:
        virtual ~IViewPresenter() = default;

        virtual bool Show(const ViewManifest& a_view) noexcept = 0;
        virtual void SetInputFocus(bool a_focused) noexcept = 0;
        virtual void Hide(std::string_view a_viewId) noexcept = 0;
        virtual void Tick() noexcept = 0;
    };
}
