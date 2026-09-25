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
		if (m_pendingRing) {
			CloseHandles(*m_pendingRing);
		}
	}

	void SharedFrameConsumer::AnnounceRing(SharedRingDesc a_ring)
	{
		std::scoped_lock lock(m_mutex);
		if (m_pendingRing) {
			CloseHandles(*m_pendingRing);
		}
		a_ring.generation = ++m_nextGeneration;
		m_state.BeginRing(a_ring);
		m_pendingRing = a_ring;
	}

	void SharedFrameConsumer::Disconnect()
	{
		std::scoped_lock lock(m_mutex);
		m_state.Disconnect();
		if (m_pendingRing) {
			CloseHandles(*m_pendingRing);
		}
		m_pendingRing.reset();
	}

	bool SharedFrameConsumer::HasPendingRing() const
	{
		std::scoped_lock lock(m_mutex);
		return m_pendingRing.has_value();
	}

	std::optional<SharedRingDesc> SharedFrameConsumer::TakeRingIfIdle()
	{
		std::scoped_lock lock(m_mutex);
		if (m_state.HasReads()) return std::nullopt;
		return std::exchange(m_pendingRing, std::nullopt);
	}
}
