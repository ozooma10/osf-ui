#include "ViewRuntime.h"

namespace Runtime
{
    void ViewRuntime::ReplaceViews(std::vector<ViewManifest> a_views)
    {
        presentation.CloseAll();
        catalog.Replace(std::move(a_views));
    }

    ViewOperationResult ViewRuntime::OpenView(std::string_view a_id)
    {
        const auto* view = catalog.Find(a_id);
        if (!view) {
            return ViewOperationResult::UnknownView;
        }

        return presentation.Open(*view) ? ViewOperationResult::Changed : ViewOperationResult::Unchanged;
    }

    ViewOperationResult ViewRuntime::CloseView(std::string_view a_id)
    {
        if (!catalog.Find(a_id)) {
            return ViewOperationResult::UnknownView;
        }

        return presentation.Close(a_id) ? ViewOperationResult::Changed : ViewOperationResult::Unchanged;
    }

    bool ViewRuntime::CloseActiveMenu()
    {
        return presentation.CloseActiveMenu();
    }

    void ViewRuntime::CloseAllViews()
    {
        presentation.CloseAll();
    }

    std::span<const ViewManifest> ViewRuntime::Views() const
    {
        return catalog.All();
    }

    PresentationSnapshot ViewRuntime::Presentation() const
    {
        return {
            .activeMenu = presentation.ActiveMenu(),
            .openViewIds = presentation.OpenViewIds(),
            .capturesInput = presentation.CapturesInput(),
            .pausesGame = presentation.PausesGame()
        };
    }
}