#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

namespace OSFUI::NativeMainThreadQueue
{
	enum class PostResult
	{
		Queued,
		RanInline,
		Unavailable,
	};

	PostResult Post(std::function<void()> a_task, std::string_view a_label, std::function<void()> a_onDrop = {});
}
