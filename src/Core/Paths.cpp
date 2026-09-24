#include "Core/Paths.h"

#include "Core/Utf8Path.h"
#include "Core/Version.h"

#include "Win32Util.h"

namespace OSFUI::Paths
{
	namespace
	{
		std::filesystem::path g_dataDir;
	}

	bool Initialize()
	{
		const auto gamePath = osfui::win32::ModulePath();
		if (gamePath.empty()) {
			REX::ERROR("Paths: cannot locate the game executable (Win32 error {})", ::GetLastError());
			return false;
		}
		g_dataDir = gamePath.parent_path() / "Data" / "SFSE" / "Plugins" / kDataFolderName;
		REX::INFO("Paths: data dir = {}", Utf8Path(g_dataDir));

		std::error_code ec;
		if (!std::filesystem::exists(g_dataDir, ec)) {
			REX::WARN("Paths: OSF UI data directory does not exist ({}); no packaged views are available", Utf8Path(g_dataDir));
		}
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
