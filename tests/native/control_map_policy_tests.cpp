#include "Bindings/ControlMapPolicy.h"

#include <iostream>

namespace
{
	int failures = 0;

	void Check(bool a_value, const char* a_message)
	{
		if (!a_value) {
			std::cerr << "FAIL: " << a_message << '\n';
			++failures;
		}
	}
}

int main()
{
	using namespace OSFUI;
	using namespace OSFUI::ControlMapPolicy;

	const auto main = Classify(0x00, "MainGameplay");
	Check(main.classification == Classification::Core, "MainGameplay is core");
	Check(ModesOverlap(main.definiteModes, ModeBit(GameplayMode::OnFoot)), "MainGameplay is definitely on-foot");
	Check(ModesOverlap(main.definiteModes, ModeBit(GameplayMode::Ship)), "MainGameplay is definitely ship");
	Check(ModesOverlap(main.possibleModes, ModeBit(GameplayMode::Vehicle)), "MainGameplay may be vehicle");

	Check(Classify(0x49, "Vehicle").classification == Classification::Special, "Vehicle is special");
	Check(Classify(0x28, "StarMap").classification == Classification::Menu, "StarMap family is menu");
	Check(Classify(0x45, "BasicMenuNav_JustCancel").classification == Classification::Menu, "BasicMenuNav family is menu");
	Check(Classify(0x0B, "LeftThumbstick").classification == Classification::Unknown, "unproven context stays unknown");

	const std::uint8_t vehicle[] = { 0x00, 0x21, 0x20, 0x49 };
	Check(DeriveMode(vehicle) == GameplayMode::Vehicle, "vehicle wins precedence");
	const std::uint8_t ship[] = { 0x00, 0x20, 0x21 };
	Check(DeriveMode(ship) == GameplayMode::Ship, "ship wins over zero-g and MainGameplay");
	const std::uint8_t zeroG[] = { 0x00, 0x20 };
	Check(DeriveMode(zeroG) == GameplayMode::ZeroG, "zero-g wins over MainGameplay");
	const std::uint8_t onFoot[] = { 0x00 };
	Check(DeriveMode(onFoot) == GameplayMode::OnFoot, "MainGameplay alone is on-foot");
	const std::uint8_t unknown[] = { 0x18 };
	Check(!DeriveMode(unknown), "menu-only stack has unknown gameplay mode");

	return failures;
}
