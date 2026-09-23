#include "Core/NativeMainThreadQueue.h"

#include "RE/B/BSService.h"

namespace OSFUI::NativeMainThreadQueue
{
	namespace
	{
		class GuardedTask final : public RE::BSService::QueuedDelegate
		{
		public:
			GuardedTask(std::function<void()> a_task, std::string_view a_label, std::function<void()> a_onDrop) :
				_task(std::move(a_task)), _label(a_label), _onDrop(std::move(a_onDrop)) {}

			void Run() override
			{
				const auto drainOwnerThreadId = RE::BSService::TaskQueue::GetDrainOwnerThreadID();
				const auto currentThreadId = REX::W32::GetCurrentThreadId();
				if (currentThreadId != drainOwnerThreadId) {
					if (_onDrop) {
						try {
							_onDrop();
						} catch (...) {
							// Recovery must not escape the engine queue drain.
						}
					}
					REX::CRITICAL("NativeMainThreadQueue: dropped '{}' on thread {} (drain owner {})", _label, currentThreadId, drainOwnerThreadId);
					return;
				}

				try {
					_task();
				} catch (const std::exception& e) {
					REX::ERROR("NativeMainThreadQueue: '{}' threw '{}'; payload stopped", _label, e.what());
				} catch (...) {
					REX::ERROR("NativeMainThreadQueue: '{}' threw an unknown exception; payload stopped", _label);
				}
			}

		private:
			std::function<void()> _task;
			std::string _label;
			std::function<void()> _onDrop;
		};

		bool InsideDrain()
		{
			const auto owner = RE::BSService::TaskQueue::GetDrainOwnerThreadID();
			return owner != 0 && REX::W32::GetCurrentThreadId() == owner;
		}
	}

	PostResult Post(std::function<void()> a_task, std::string_view a_label, std::function<void()> a_onDrop)
	{
		if (InsideDrain()) {
			a_task();
			return PostResult::RanInline;
		}

		auto* queue = RE::BSService::TaskQueue::GetSingleton();
		if (!queue) {
			return PostResult::Unavailable;
		}

		RE::BSService::QueuedDelegate* task = new GuardedTask(std::move(a_task), a_label, std::move(a_onDrop));
		queue->QueueTask(task);
		if (!task) {
			return PostResult::Queued;
		}

		delete task;
		return PostResult::Unavailable;
	}
}
