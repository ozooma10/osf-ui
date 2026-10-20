#pragma once
#include <filesystem>
namespace OSFUI::Extension
{
    const std::filesystem::path& DataDir();
    bool Connect();
}
