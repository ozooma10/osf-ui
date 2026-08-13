#pragma once

#include <string>

namespace Papyrus
{
	using ViewRequestHandler = bool (*)(std::string);

	void SetViewRequestHandlers(ViewRequestHandler a_openHandler, ViewRequestHandler a_closeHandler);

	bool RegisterFunctions();
}
