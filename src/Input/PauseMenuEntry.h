#pragma once

#include <string>

namespace OSFUI
{
	class PauseMenuEntry
	{
	public:
		static void Configure(std::string a_label, std::string a_viewId);
		static void Reconcile();
	};
}
