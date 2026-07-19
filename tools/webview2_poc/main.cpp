#include "PocApp.h"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE a_instance, HINSTANCE, PWSTR, int a_showCommand)
{
	PocApp app;
	return app.Run(a_instance, a_showCommand);
}
