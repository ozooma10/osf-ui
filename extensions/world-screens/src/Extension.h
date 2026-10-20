#pragma once
#include <filesystem>
namespace OSFUI::API::WorldScreens { struct IScreens; }
namespace OSFUI::Extension
{
    const std::filesystem::path& DataDir();
    API::WorldScreens::IScreens& Screens();
    bool Connect();
}
