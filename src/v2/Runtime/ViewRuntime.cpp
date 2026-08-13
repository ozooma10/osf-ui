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

        const auto previousMenu = presentation.ActiveMenu();

        if(!presentation.Open(*view)) {
            return ViewOperationResult::Unchanged;
        }

        if(view->kind == ViewKind::Menu && previousMenu && *previousMenu != view->id) {
            if(const auto* previousView = catalog.Find(*previousMenu)) {
                QueuePresentationCommand(ViewPresentationAction::Hide, *previousView);
            }
        }

        QueuePresentationCommand(ViewPresentationAction::Show, *view);
        return ViewOperationResult::Changed;
    }

    ViewOperationResult ViewRuntime::CloseView(std::string_view a_id)
    {
        const auto* view = catalog.Find(a_id);
        if(!view) {
            return ViewOperationResult::UnknownView;
        }

        if(!presentation.Close(a_id)) {
            return ViewOperationResult::Unchanged;
        }

        QueuePresentationCommand(ViewPresentationAction::Hide, *view);
        return ViewOperationResult::Changed;
    }

    bool ViewRuntime::CloseActiveMenu()
    {
        const auto activeMenu = presentation.ActiveMenu();
        if(!activeMenu) {
            return false;
        }

        const auto* view = catalog.Find(*activeMenu);

        if(!presentation.CloseActiveMenu()) {
            return false;
        }
        if(view) {
            QueuePresentationCommand(ViewPresentationAction::Hide, *view);
        }
        return true;
    }

    void ViewRuntime::CloseAllViews()
    {
        const auto openViewIds = presentation.OpenViewIds();

        presentation.CloseAll();

        for(const auto& id : openViewIds) {
            if(const auto* view = catalog.Find(id)) {
                QueuePresentationCommand(ViewPresentationAction::Hide, *view);
            }
        }
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

    std::vector<ViewPresentationCommand> ViewRuntime::TakePresentationCommands()
    {
        std::vector<ViewPresentationCommand> commands;
        commands.swap(pendingCommands);
        return commands;
    }

    void ViewRuntime::QueuePresentationCommand(ViewPresentationAction a_action, const ViewManifest& a_view)
    {
        pendingCommands.push_back({
            .action = a_action,
            .view = a_view
        });
    }
}