#include "v2/Platform/NativeMainThreadQueue.h"

#include "RE/B/BSService.h"

namespace Platform::NativeMainThreadQueue
{
    namespace
    {
        void InvokeSafely(NativeMainThreadQueue::Task& a_task, std::string_view a_label)
        {
            try {
                a_task();
            } catch (const std::exception& e) {
                REX::ERROR("NativeMainThreadQueue: exception while executing task '{}': {}", a_label, e.what());
            } catch (...) {
                REX::ERROR("NativeMainThreadQueue: unknown exception while executing task '{}'", a_label);
            }
        }

        void InvokeDropSafely(NativeMainThreadQueue::Task& a_onDrop, std::string_view a_label)
        {
            if(!a_onDrop) {
                return;
            }
            try {
                a_onDrop();
            } catch (...) {
                REX::ERROR("NativeMainThreadQueue: unknown exception while dropping task '{}'", a_label);
            }
        }

        class GuardedTask final : public RE::BSService::QueuedDelegate
        {
        public:
            GuardedTask(NativeMainThreadQueue::Task a_task, std::string_view a_label, NativeMainThreadQueue::Task a_onDrop) :
                _task(std::move(a_task)), _label(a_label), _onDrop(std::move(a_onDrop)) {}

            void Run() override
            {
                const auto currentThread = REX::W32::GetCurrentThreadId();
                const auto ownerThread = RE::BSService::TaskQueue::GetDrainOwnerThreadID();

                if (ownerThread == 0 || currentThread != ownerThread) {
                    InvokeDropSafely(_onDrop, _label);

                    REX::CRITICAL("[NativeMainThreadQueue] dropped '{}' because it ran outside the native drain: tid={}, ownerTid={}", _label, currentThread, ownerThread);
                    return;
                }

                InvokeSafely(_task, _label);
            }

        private:
            NativeMainThreadQueue::Task _task;
            std::string                 _label;
            NativeMainThreadQueue::Task _onDrop;
        };
    }

	bool IsOnDrainThread() noexcept
	{
		const auto ownerThread = RE::BSService::TaskQueue::GetDrainOwnerThreadID();

		return ownerThread != 0 && ownerThread == REX::W32::GetCurrentThreadId();
	}

    PostResult Post(Task a_task, std::string_view a_label, Task a_onDrop)
    {
        if (!a_task) {
            REX::ERROR("[NativeMainThreadQueue] refused empty task '{}'", a_label);
            return PostResult::Unavailable;
        }

        if (IsOnDrainThread()) {
            InvokeSafely(a_task, a_label);
            return PostResult::RanInline;
        }

        auto* queue = RE::BSService::TaskQueue::GetSingleton();
        if (!queue) { return PostResult::Unavailable; }

        RE::BSService::QueuedDelegate* delegate = new GuardedTask{ std::move(a_task), a_label, std::move(a_onDrop) };
        queue->QueueTask(delegate);
        if (!delegate) {
            return PostResult::Queued; // QueueTask stole the reference. The engine now owns it.
        }

        // The queue refused it. Delete directly so the payload does not run on this potentially unsafe caller thread.
        delete delegate;
        return PostResult::Unavailable;
    }

    bool IsAvailable() noexcept
    {
        if (IsOnDrainThread()) {
            return true;
        }
        return RE::BSService::TaskQueue::GetSingleton() != nullptr && RE::BSService::TaskQueue::IsQueueEnabled();
    }

    const char* ToString(PostResult a_result) noexcept
    {
        switch (a_result) {
        case PostResult::Queued:
            return "queued";
        case PostResult::RanInline:
            return "ran-inline";
        case PostResult::Unavailable:
            return "unavailable";
        default:
            return "unknown";
        }
    }
}