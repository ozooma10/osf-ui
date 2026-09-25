#pragma once

#include "Render/SharedTextureTransport.h"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace OSFUI
{
	// All access is serialized by SharedFrameConsumer. Pending/current own a selection reference; recorded reads own references until GPU completion.
	// A retired frame is one held only by reads. Its final release creates one host acknowledgement. Neither metadata snapshots nor submission release it.
	class SharedFrameState
	{
	public:
		SharedFrameState() = default;
		SharedFrameState(const SharedFrameState&) = delete;
		SharedFrameState& operator=(const SharedFrameState&) = delete;
		SharedFrameState(SharedFrameState&&) = default;
		SharedFrameState& operator=(SharedFrameState&&) = default;
		using Releases = std::array<std::uint64_t, SharedRingDesc::kMaxSlots>;
		enum class PublishResult { Accepted, Discarded, Invalid };

		void BeginRing(const SharedRingDesc& a_ring)
		{
			Invalidate();
			m_generation = std::make_shared<Generation>();
			m_generation->id = a_ring.generation;
			m_generation->slotCount = a_ring.slotCount;
			m_generation->width = a_ring.width;
			m_generation->height = a_ring.height;
		}

		void Disconnect()
		{
			Invalidate();
			m_generation.reset(); // old reads can finish, but cannot ack a new host
		}

		void SetPresentation(std::uint64_t a_epoch, bool a_acceptFrames)
		{
			if (m_epoch != a_epoch || !a_acceptFrames) {
				Invalidate();
			}
			m_epoch = a_epoch;
			m_acceptFrames = a_acceptFrames;
		}

		void SetVisible(bool a_visible)
		{
			m_visible = a_visible;
			if (!a_visible) {
				Invalidate();
			}
		}

		PublishResult Publish(std::uint32_t a_slot, std::uint64_t a_serial, std::uint32_t a_width, std::uint32_t a_height, std::uint64_t a_epoch)
		{
			if (!m_generation || a_slot >= m_generation->slotCount || !a_serial) return PublishResult::Invalid;
			if (a_serial <= m_generation->lastSerial) return PublishResult::Discarded;
			const auto held = [&](const auto& a_frame) {
				return a_frame && a_frame->generation == m_generation && a_frame->view.sharedSlot == a_slot;
			};
			if (held(m_pending) || held(m_current) || std::ranges::any_of(m_reads, [&](const Read& a_read) { return held(a_read.frame); })) {
				return PublishResult::Invalid;
			}
			m_generation->lastSerial = a_serial;
			if (!m_acceptFrames || a_epoch != m_epoch || a_width != m_generation->width || a_height != m_generation->height) {
				m_generation->released[a_slot] = a_serial;
				return PublishResult::Discarded;
			}
			const FrameBufferView view{ a_width, a_height, m_generation->id, a_serial, a_slot };
			m_pending = std::make_shared<Frame>(view, m_generation);
			m_latest = view;
			return PublishResult::Accepted;
		}

		std::optional<FrameBufferView> Latest() const { return m_latest; }

		// Reserve a read before the caller writes any GPU commands. Failure leaves selection unchanged. A list can read several frames; each is retained.
		std::optional<FrameBufferView> Record(std::uintptr_t a_list, bool a_first, std::uint64_t a_activeGeneration, std::uint64_t a_produced)
		{
			if (!m_visible || !a_list) return std::nullopt;
			const bool promote = a_first && m_pending && m_pending->view.ringGeneration == a_activeGeneration && m_pending->view.frameIndex <= a_produced;
			const auto& selected = promote ? m_pending : m_current;
			if (!selected || selected->view.ringGeneration != a_activeGeneration) return std::nullopt;
			if (!std::ranges::any_of(m_reads, [&](const Read& a_read) {
				return a_read.list == a_list && a_read.frame == selected;
			})) {
				m_reads.push_back({ .frame = selected, .list = a_list });
			}
			if (promote) {
				m_current = std::exchange(m_pending, {});
			}
			return m_current->view;
		}

		bool HasRecorded(std::span<const std::uintptr_t> a_lists) const
		{
			return std::ranges::any_of(m_reads, [&](const Read& a_read) {
				return a_read.list && std::ranges::find(a_lists, a_read.list) != a_lists.end();
			});
		}
		bool HasRecorded() const
		{
			return std::ranges::any_of(m_reads, [](const Read& a_read) { return a_read.list != 0; });
		}

		// queue == 0 means the completion signal failed: retain those reads until teardown, never turn an unknown GPU completion into a host release.
		void Submitted(std::span<const std::uintptr_t> a_lists, std::uintptr_t a_queue, std::uint64_t a_value)
		{
			for (auto& read : m_reads) {
				if (read.list && std::ranges::find(a_lists, read.list) != a_lists.end()) {
					read.list = 0;
					read.queue = a_queue;
					read.completion = a_value;
				}
			}
		}

		void Completed(std::uintptr_t a_queue, std::uint64_t a_value)
		{
			if (!a_queue || a_value == UINT64_MAX) return; // device removed is not completion
			std::erase_if(m_reads, [&](const Read& a_read) {
				return !a_read.list && a_read.queue == a_queue && a_read.completion <= a_value;
			});
		}

		bool HasReads() const { return !m_reads.empty(); }
		Releases TakeReleases() { return m_generation ? std::exchange(m_generation->released, {}) : Releases{}; }

	private:
		struct Generation
		{
			std::uint64_t id{ 0 };
			std::uint32_t slotCount{ 0 }, width{ 0 }, height{ 0 };
			std::uint64_t lastSerial{ 0 };
			Releases released{};
		};
		struct Frame
		{
			Frame(FrameBufferView a_view, std::shared_ptr<Generation> a_generation) :
				view(a_view), generation(std::move(a_generation)) {}
			~Frame()
			{
				auto& released = generation->released[view.sharedSlot];
				released = (std::max)(released, view.frameIndex);
			}
			Frame(const Frame&) = delete;
			FrameBufferView view;
			std::shared_ptr<Generation> generation;
		};
		struct Read
		{
			std::shared_ptr<Frame> frame;
			std::uintptr_t list{ 0 }, queue{ 0 };
			std::uint64_t completion{ 0 };
		};
		void Invalidate()
		{
			m_pending.reset();
			m_current.reset();
			m_latest.reset();
		}
		std::shared_ptr<Generation> m_generation;
		std::shared_ptr<Frame> m_pending, m_current;
		std::vector<Read> m_reads;
		std::optional<FrameBufferView> m_latest;
		std::uint64_t m_epoch{ 0 };
		bool m_acceptFrames{ false }, m_visible{ false };
	};
}
