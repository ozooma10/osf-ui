#pragma once


#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#	define NOMINMAX
#endif
#include <Windows.h>

namespace osfui::wv2
{
	class Pipe
	{
	public:
		Pipe() = default;
		~Pipe() { Close(); }
		Pipe(const Pipe&) = delete;
		Pipe& operator=(const Pipe&) = delete;

		void PrepareForOpen();

		bool CreateServer(const std::wstring& a_name);
		bool WaitForClient(std::uint32_t a_timeoutMs);
		bool CreateServerAndWait(const std::wstring& a_name, std::uint32_t a_timeoutMs);

		// Client: connect to \\.\pipe\<a_name>, retrying until a_timeoutMs.
		bool Connect(const std::wstring& a_name, std::uint32_t a_timeoutMs);

		bool ReadMessage(std::string& a_payload, std::uint32_t a_timeoutMs = INFINITE);

		// Framed write (thread-safe). Returns false on error.
		bool WriteMessage(const std::string& a_payload);

		void Close();

		[[nodiscard]] bool IsCreated() const;
		[[nodiscard]] bool IsOpen() const;
		[[nodiscard]] std::optional<std::uint32_t> ClientProcessId();
		[[nodiscard]] std::optional<std::uint32_t> ServerProcessId();
		[[nodiscard]] std::string LastErrorText() const;

	private:
		enum class CallKind { Open, Accept, Read, Write };
		class CallGuard;
		bool BeginCall(CallKind a_kind);
		void EndCall(CallKind a_kind);
		void CloseLocked();
		[[nodiscard]] bool IsClosing() const;
		bool ReadExact(std::uint8_t* a_buffer, std::uint32_t a_bytes, std::uint64_t a_deadline);
		bool PublishOpenHandles(HANDLE a_pipe, HANDLE a_readEvent,
			HANDLE a_writeEvent, bool a_connected);
		void SetError(const char* a_where, DWORD a_code);

		HANDLE      _pipe{ INVALID_HANDLE_VALUE };
		HANDLE      _readEvent{ nullptr };   // overlapped read (cancellable)
		HANDLE      _writeEvent{ nullptr };  // overlapped write
		std::mutex  _writeMutex;
		std::mutex  _lifecycleMutex;
		mutable std::mutex _stateMutex;
		std::condition_variable _idle;
		std::size_t _activeCalls{ 0 };
		bool _opening{ false };
		bool _accepting{ false };
		bool _readerActive{ false };
		bool _connected{ false };
		bool _closing{ true };
		mutable std::mutex _errorMutex;
		std::string _lastError;
	};
}
