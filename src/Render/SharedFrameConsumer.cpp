#include "Render/SharedFrameConsumer.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

namespace OSFUI
{
	void SharedFrameConsumer::CloseHandles(SharedRingDesc& a_ring)
	{
		for (auto*& handle : a_ring.slotHandles) {
			if (handle) {
				::CloseHandle(std::exchange(handle, nullptr));
			}
		}
		if (a_ring.produceFence) {
			::CloseHandle(std::exchange(a_ring.produceFence, nullptr));
		}
	}

	SharedFrameConsumer::~SharedFrameConsumer()
	{
		if (_pendingRing) {
			CloseHandles(*_pendingRing);
		}
	}

	void SharedFrameConsumer::AnnounceRing(SharedRingDesc a_ring)
	{
		std::scoped_lock lock(_mutex);
		if (_pendingRing) {
			CloseHandles(*_pendingRing);
		}
		a_ring.generation = ++_nextGeneration;
		_state.BeginRing(a_ring);
		_pendingRing = a_ring;
	}

	void SharedFrameConsumer::Disconnect()
	{
		std::scoped_lock lock(_mutex);
		_state.Disconnect();
		if (_pendingRing) {
			CloseHandles(*_pendingRing);
		}
		_pendingRing.reset();
	}

	bool SharedFrameConsumer::HasPendingRing() const
	{
		std::scoped_lock lock(_mutex);
		return _pendingRing.has_value();
	}

	std::optional<SharedRingDesc> SharedFrameConsumer::TakeRingIfIdle()
	{
		std::scoped_lock lock(_mutex);
		if (_state.HasReads()) return std::nullopt;
		return std::exchange(_pendingRing, std::nullopt);
	}
}
