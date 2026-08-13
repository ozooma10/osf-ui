#pragma once

#include "ViewCatalog.h"
#include "ViewPresentationCommand.h"
#include "ViewPresentationController.h"

namespace Runtime
{
    enum class ViewOperationResult
    {
        Changed,
        Unchanged,
        UnknownView
    };

    struct PresentationSnapshot
    {
        std::optional<std::string> activeMenu;
        std::vector<std::string> openViewIds;
        bool capturesInput{ false };
        bool pausesGame{ false };

        bool operator==(const PresentationSnapshot&) const = default;
    };

    class ViewRuntime
    {
    public:
        void ReplaceViews(std::vector<ViewManifest> a_views);

        ViewOperationResult OpenView(std::string_view a_id);
        ViewOperationResult CloseView(std::string_view a_id);

        bool CloseActiveMenu();
        void CloseAllViews();

        std::span<const ViewManifest> Views() const;
        PresentationSnapshot Presentation() const;

        std::vector<ViewPresentationCommand> TakePresentationCommands();

    private:
        void QueuePresentationCommand(ViewPresentationAction a_action, const ViewManifest& a_view);    

        ViewCatalog catalog;
        ViewPresentationController presentation;
        std::vector<ViewPresentationCommand> pendingCommands;
    };
}