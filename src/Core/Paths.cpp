#include "Core/Paths.h"

#include "REX/FModule.h"

namespace OSFUI::Paths
{
	namespace
	{
		std::filesystem::path g_dataDir;
	}

	void Initialize()
	{
		const std::filesystem::path gamePath{ REX::FModule::GetExecutingModule().GetFileName() };
		g_dataDir = gamePath.parent_path() / "Data" / "SFSE" / "Plugins" / "OSF" / "UI";
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
