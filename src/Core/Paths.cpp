#include "Core/Paths.h"

#include "Core/Utf8Path.h"
#include "Core/Version.h"

#include "REX/FModule.h"

namespace OSFUI::Paths
{
	namespace
	{
		std::filesystem::path g_dataDir;
	}

	bool Initialize()
	{
		const std::filesystem::path gamePath{ REX::FModule::GetExecutingModule().GetFileName() };
		g_dataDir = gamePath.parent_path() / "Data" / "SFSE" / "Plugins" / "OSF" / "UI";
		return true;
	}

	const std::filesystem::path& DataDir()
	{
		return g_dataDir;
	}

	std::filesystem::path ViewsDir()
	{
		return g_dataDir / "views";
	}
}
