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
			_generation = std::make_shared<Generation>();
			_generation->id = a_ring.generation;
			_generation->slotCount = a_ring.slotCount;
			_generation->width = a_ring.width;
			_generation->height = a_ring.height;
		}

		void Disconnect()
		{
			Invalidate();
			_generation.reset(); // old reads can finish, but cannot ack a new host
		}

		void SetPresentation(std::uint64_t a_epoch, bool a_acceptFrames)
		{
			if (_epoch != a_epoch || !a_acceptFrames) {
				Invalidate();
			}
			_epoch = a_epoch;
			_acceptFrames = a_acceptFrames;
		}

		void SetVisible(bool a_visible)
		{
			_visible = a_visible;
			if (!a_visible) {
				Invalidate();
			}
		}

		PublishResult Publish(std::uint32_t a_slot, std::uint64_t a_serial, std::uint32_t a_width, std::uint32_t a_height, std::uint64_t a_epoch)
		{
			if (!_generation || a_slot >= _generation->slotCount || !a_serial) return PublishResult::Invalid;
			if (a_serial <= _generation->lastSerial) return PublishResult::Discarded;
			const auto held = [&](const auto& a_frame) {
				return a_frame && a_frame->generation == _generation && a_frame->view.sharedSlot == a_slot;
			};
			if (held(_pending) || held(_current) || std::ranges::any_of(_reads, [&](const Read& a_read) { return held(a_read.frame); })) {
				return PublishResult::Invalid;
			}
			_generation->lastSerial = a_serial;
			if (!_acceptFrames || a_epoch != _epoch || a_width != _generation->width || a_height != _generation->height) {
				_generation->released[a_slot] = a_serial;
				return PublishResult::Discarded;
			}
			const FrameBufferView view{ a_width, a_height, _generation->id, a_serial, a_slot };
			_pending = std::make_shared<Frame>(view, _generation);
			_latest = view;
			return PublishResult::Accepted;
		}

		std::optional<FrameBufferView> Latest() const { return _latest; }

		// Reserve a read before the caller writes any GPU commands. Failure leaves selection unchanged. A list can read several frames; each is retained.
		std::optional<FrameBufferView> Record(std::uintptr_t a_list, bool a_first, std::uint64_t a_activeGeneration, std::uint64_t a_produced)
		{
			if (!_visible || !a_list) return std::nullopt;
			const bool promote = a_first && _pending && _pending->view.ringGeneration == a_activeGeneration && _pending->view.frameIndex <= a_produced;
			const auto& selected = promote ? _pending : _current;
			if (!selected || selected->view.ringGeneration != a_activeGeneration) return std::nullopt;
			if (!std::ranges::any_of(_reads, [&](const Read& a_read) {
				return a_read.list == a_list && a_read.frame == selected;
			})) {
				try {
					_reads.push_back({ .frame = selected, .list = a_list });
				} catch (...) {
					return std::nullopt;
				}
			}
			if (promote) {
				_current = std::exchange(_pending, {});
			}
			return _current->view;
		}

		bool HasRecorded(std::span<const std::uintptr_t> a_lists) const
		{
			return std::ranges::any_of(_reads, [&](const Read& a_read) {
				return a_read.list && std::ranges::find(a_lists, a_read.list) != a_lists.end();
			});
		}
		bool HasRecorded() const
		{
			return std::ranges::any_of(_reads, [](const Read& a_read) { return a_read.list != 0; });
		}

		// queue == 0 means the completion signal failed: retain those reads until teardown, never turn an unknown GPU completion into a host release.
		void Submitted(std::span<const std::uintptr_t> a_lists, std::uintptr_t a_queue, std::uint64_t a_value)
		{
			for (auto& read : _reads) {
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
			std::erase_if(_reads, [&](const Read& a_read) {
				return !a_read.list && a_read.queue == a_queue && a_read.completion <= a_value;
			});
		}

		bool HasReads() const { return !_reads.empty(); }
		Releases TakeReleases() { return _generation ? std::exchange(_generation->released, {}) : Releases{}; }

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
			_pending.reset();
			_current.reset();
			_latest.reset();
		}
		std::shared_ptr<Generation> _generation;
		std::shared_ptr<Frame> _pending, _current;
		std::vector<Read> _reads;
		std::optional<FrameBufferView> _latest;
		std::uint64_t _epoch{ 0 };
		bool _acceptFrames{ false }, _visible{ false };
	};
}
