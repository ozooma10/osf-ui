#pragma once

namespace OSFUI::Log
{
	void WarnOnce(std::once_flag& a_flag, std::string_view a_message);

	[[nodiscard]] bool DevMode();
	void SetDevMode(bool a_enabled);
}
