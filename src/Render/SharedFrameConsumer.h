#pragma once

#include "Render/SharedFrameState.h"

#include <mutex>
#include <atomic>

namespace OSFUI
{
	// One owner shared by the transport and compositor. No pipe writes or GPU waits under this lock. Ring handles wait here until all old reads complete.
	class SharedFrameConsumer
	{
	public:
		~SharedFrameConsumer();
		void AnnounceRing(SharedRingDesc a_ring);
		void Disconnect();
		bool HasPendingRing() const;
		std::optional<SharedRingDesc> TakeRingIfIdle();
		static void CloseHandles(SharedRingDesc& a_ring);

		void SetPresentation(std::uint64_t a_epoch, bool a_accept)
		{
			std::scoped_lock lock(_mutex);
			_state.SetPresentation(a_epoch, a_accept);
		}
		void SetVisible(bool a_visible)
		{
			std::scoped_lock lock(_mutex);
			_state.SetVisible(a_visible);
		}
		SharedFrameState::PublishResult Publish(std::uint32_t a_slot, std::uint64_t a_serial, std::uint32_t a_width, std::uint32_t a_height, std::uint64_t a_epoch)
		{
			std::scoped_lock lock(_mutex);
			return _state.Publish(a_slot, a_serial, a_width, a_height, a_epoch);
		}
		std::optional<FrameBufferView> Latest() const
		{
			std::scoped_lock lock(_mutex);
			return _state.Latest();
		}
		std::optional<FrameBufferView> Record(std::uintptr_t a_list, bool a_first, std::uint64_t a_generation, std::uint64_t a_produced)
		{
			std::scoped_lock lock(_mutex);
			auto result = _state.Record(a_list, a_first, a_generation, a_produced);
			_hasRecorded.store(_state.HasRecorded(), std::memory_order_release);
			return result;
		}
		bool HasRecorded() const { return _hasRecorded.load(std::memory_order_acquire); }
		bool HasRecorded(std::span<const std::uintptr_t> a_lists) const
		{
			std::scoped_lock lock(_mutex);
			return _state.HasRecorded(a_lists);
		}
		void Submitted(std::span<const std::uintptr_t> a_lists, std::uintptr_t a_queue, std::uint64_t a_value)
		{
			std::scoped_lock lock(_mutex);
			_state.Submitted(a_lists, a_queue, a_value);
			_hasRecorded.store(_state.HasRecorded(), std::memory_order_release);
		}
		void Completed(std::uintptr_t a_queue, std::uint64_t a_value)
		{
			std::scoped_lock lock(_mutex);
			_state.Completed(a_queue, a_value);
		}
		bool HasReads() const
		{
			std::scoped_lock lock(_mutex);
			return _state.HasReads();
		}
		SharedFrameState::Releases TakeReleases()
		{
			std::scoped_lock lock(_mutex);
			return _state.TakeReleases();
		}

	private:
		mutable std::mutex _mutex;
		SharedFrameState _state;
		std::atomic_bool _hasRecorded{ false }; // queue-hook fast path; state owns the truth
		std::optional<SharedRingDesc> _pendingRing;
		std::uint64_t _nextGeneration{ 0 };
	};
}
