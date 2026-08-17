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

	struct QueueState
	{
		std::uintptr_t singleton{ 0 };
		std::uint32_t currentThreadId{ 0 };
		std::uint32_t drainOwnerThreadId{ 0 };
		bool queueEnabled{ false };
		bool insideDrain{ false };
	};

	QueueState SnapshotState();
	bool IsAvailable();

	PostResult Post(std::function<void()> a_task, std::string_view a_label, std::function<void()> a_onDrop = {});

	const char* ToString(PostResult a_result);
}
