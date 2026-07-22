#pragma once

// Desktop-test stand-in for CommonLibSF's FormType enum: just the members the
// form-serialization tests exercise, at their real numeric values. Never used
// by the real plugin build (the lib/commonlibsf include path wins there).

namespace RE
{
	enum class FormType
	{
		kNONE = 0x00,
		kKYWD = 0x04,
		kWEAP = 0x30,
		kFLST = 0x69,
	};
}
