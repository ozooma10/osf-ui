#pragma once

#include "BrowserPipe.h"
#include "BrowserProtocol.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace osfui::browser
{
	// One FIFO for every host message, including ring announcements, frames and logs.
	// Enqueue never performs pipe I/O. Only Stop may wait, during host teardown.
	// A queued frame still owns its slot until the game ACKs it; never coalesce frames.
	class PipeWriter
	{
	public:
		enum class Result { Queued, InvalidPayload, Full, Closed };
		enum class Failure { None, Overflow, Write };

		// Own handles duplicated into the peer until its complete announcement is written.
		// The peer process handle is borrowed and must outlive Stop (including its join).
		class HandleBatch
		{
		public:
			explicit HandleBatch(HANDLE a_process, std::size_t a_capacity) : m_process(a_process)
			{
				m_handles.reserve(a_capacity);
			}
			~HandleBatch() { for (const auto handle : m_handles) Close(handle); }
			HandleBatch(const HandleBatch&) = delete;
			HandleBatch& operator=(const HandleBatch&) = delete;

			void Add(HANDLE a_handle)
			{
				try { m_handles.push_back(a_handle); }
				catch (...) { Close(a_handle); throw; }
			}
			void Release() { m_handles.clear(); } // The peer now owns the announced handles.

		private:
			void Close(HANDLE a_handle) const
			{
				HANDLE local = nullptr;
				if (::DuplicateHandle(m_process, a_handle, ::GetCurrentProcess(), &local,
					0, FALSE, DUPLICATE_CLOSE_SOURCE)) ::CloseHandle(local);
			}
			HANDLE m_process;
			std::vector<HANDLE> m_handles;
		};

		explicit PipeWriter(Pipe& a_pipe, std::function<void()> a_failed,
			std::size_t a_maxMessages = 1024, std::size_t a_maxBytes = 16u * 1024u * 1024u) :
			m_pipe(a_pipe), m_failed(std::move(a_failed)),
			m_maxMessages(a_maxMessages), m_maxBytes(a_maxBytes)
		{}
		~PipeWriter() { Stop(); }
		PipeWriter(const PipeWriter&) = delete;
		PipeWriter& operator=(const PipeWriter&) = delete;

		// Single-use. Start/Stop belong to the host owner; producers may enqueue concurrently.
		void Start()
		{
			std::scoped_lock lock(m_mutex);
			m_accepting = true;
			m_thread = std::thread([this] { Run(); });
		}

		// Takes ownership even on rejection. A failed framed write cannot be parsed by the
		// peer; only Written transfers ownership, never merely accepting into this queue.
		Result Enqueue(std::string a_payload, std::unique_ptr<HandleBatch> a_handles = {})
		{
			if (a_payload.empty() || a_payload.size() > kMaxMessageBytes) return Result::InvalidPayload;
			std::unique_lock lock(m_mutex);
			if (!m_accepting) return Result::Closed;
			if (m_queue.size() >= m_maxMessages || a_payload.size() > m_maxBytes - m_queuedBytes) {
				FailLocked(Failure::Overflow);
				lock.unlock();
				m_ready.notify_one();
				m_failed(); // Wake the owner to close the transport; never wait for I/O here.
				return Result::Full;
			}
			m_queue.push_back({ std::move(a_payload), std::move(a_handles) });
			m_queuedBytes += m_queue.back().payload.size();
			lock.unlock();
			m_ready.notify_one();
			return Result::Queued;
		}

		Failure Failed() const { return m_failure.load(std::memory_order_acquire); }

		// Give normal shutdown/bye a bounded drain, then cancel a blocked write before joining.
		// The pipe also cancels its reader, which the owner must join before destroying it.
		void Stop(std::chrono::milliseconds a_drain = std::chrono::milliseconds::zero())
		{
			{
				std::unique_lock lock(m_mutex);
				m_accepting = false;
				m_ready.notify_one();
				if (m_thread.joinable() && a_drain.count() > 0) {
					m_done.wait_for(lock, a_drain, [this] { return m_finished; });
				}
				m_abort = true;
				m_queue.clear();
				m_queuedBytes = 0;
			}
			m_ready.notify_one();
			m_pipe.Close();
			if (m_thread.joinable()) m_thread.join();
		}

	private:
		struct Message
		{
			std::string payload;
			std::unique_ptr<HandleBatch> handles;
		};

		void FailLocked(Failure a_failure)
		{
			m_failure.store(a_failure, std::memory_order_release);
			m_accepting = false;
			m_abort = true;
			m_queue.clear();
			m_queuedBytes = 0;
		}

		void Run()
		{
			for (;;) {
				Message message;
				{
					std::unique_lock lock(m_mutex);
					m_ready.wait(lock, [this] { return m_abort || !m_queue.empty() || !m_accepting; });
					if (m_abort || m_queue.empty()) break;
					message = std::move(m_queue.front());
					m_queue.pop_front();
					m_queuedBytes -= message.payload.size();
				}
				if (m_pipe.WriteMessage(message.payload) != Pipe::WriteResult::Written) {
					bool notify = false;
					{
						std::scoped_lock lock(m_mutex);
						if (!m_abort) {
							FailLocked(Failure::Write);
							notify = true;
						}
					}
					if (notify) m_failed();
					break;
				}
				if (message.handles) message.handles->Release();
			}
			{
				std::scoped_lock lock(m_mutex);
				m_finished = true;
			}
			m_done.notify_one();
		}

		Pipe& m_pipe;
		std::function<void()> m_failed; // Must only signal the owner, not stop/join this writer.
		const std::size_t m_maxMessages, m_maxBytes; // Queued payloads; at most one more is in flight.
		std::mutex m_mutex;
		std::condition_variable m_ready, m_done;
		std::deque<Message> m_queue;
		std::size_t m_queuedBytes{};
		bool m_accepting{}, m_abort{}, m_finished{};
		std::atomic<Failure> m_failure{ Failure::None };
		std::thread m_thread;
	};
}
