#pragma once

namespace OSFUI::Log
{
	void WarnOnce(std::once_flag& a_flag, std::string_view a_message);

	[[nodiscard]] bool DebugEnabled();
	void SetDebugLogging(bool a_enabled);
}
