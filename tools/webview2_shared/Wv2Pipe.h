#pragma once

// Message-framed named-pipe transport for the OSF UI <-> webview2 host IPC.
// Logging-free (shared between the SFSE plugin and standalone tools); failures
// surface through return values + LastErrorText().
//
// Threading: WriteMessage is serialized by an internal mutex and safe from any
// thread. ReadMessage blocks and must run on a single reader thread only.
// Close() cancels pending I/O and unblocks the reader.

#include <cstdint>
#include <mutex>
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

		// Server: create \\.\pipe\<a_name> ACL'd to the calling user only, then
		// wait up to a_timeoutMs for one client. One instance, one client.
		bool CreateServerAndWait(const std::wstring& a_name, std::uint32_t a_timeoutMs);

		// Client: connect to \\.\pipe\<a_name>, retrying until a_timeoutMs.
		bool Connect(const std::wstring& a_name, std::uint32_t a_timeoutMs);

		// Blocking framed read. Returns false on EOF/error/Close().
		bool ReadMessage(std::string& a_payload);

		// Framed write (thread-safe). Returns false on error.
		bool WriteMessage(const std::string& a_payload);

		void Close();

		[[nodiscard]] bool IsOpen() const { return _pipe != INVALID_HANDLE_VALUE; }
		[[nodiscard]] const std::string& LastErrorText() const { return _lastError; }

	private:
		bool ReadExact(std::uint8_t* a_buffer, std::uint32_t a_bytes);
		void SetError(const char* a_where, DWORD a_code);

		HANDLE      _pipe{ INVALID_HANDLE_VALUE };
		HANDLE      _readEvent{ nullptr };   // overlapped read (cancellable)
		HANDLE      _writeEvent{ nullptr };  // overlapped write
		std::mutex  _writeMutex;
		std::string _lastError;
		volatile bool _closing{ false };
	};
}
