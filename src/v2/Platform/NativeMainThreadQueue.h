#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

namespace Platform::NativeMainThreadQueue
{
	using Task = std::function<void()>;

	enum class PostResult : std::uint8_t
	{
		Queued,
		RanInline,
		Unavailable,
	};

	// True only while the current thread owns the engine's native task drain.
	bool IsOnDrainThread() noexcept;

	// Advisory only. Availability may change immediately after this returns.
	bool IsAvailable() noexcept;

	PostResult Post(Task a_task, std::string_view a_label, Task a_onDrop = {});

	const char* ToString(PostResult a_result) noexcept;
}