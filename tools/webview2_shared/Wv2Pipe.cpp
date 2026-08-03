#include "Wv2Pipe.h"

#include "Wv2Protocol.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <sddl.h>

namespace osfui::wv2
{
	namespace
	{
		// Security descriptor granting full pipe access to the OWNER only.
		// "D:P(A;;GA;;;OW)" — DACL, protected, allow generic-all to owner
		// rights. Blocks other users (and lower-integrity squatters) from
		// connecting to the frame/input channel.
		constexpr wchar_t kOwnerOnlySddl[] = L"D:P(A;;GA;;;OW)";
	}

	class Pipe::CallGuard
	{
	public:
		CallGuard(Pipe& a_owner, CallKind a_kind) :
			_owner(&a_owner), _kind(a_kind)
		{}
		~CallGuard() { Release(); }
		CallGuard(const CallGuard&) = delete;
		CallGuard& operator=(const CallGuard&) = delete;

		void Release()
		{
			if (_owner) {
				_owner->EndCall(_kind);
				_owner = nullptr;
			}
		}

	private:
		Pipe* _owner;
		CallKind _kind;
	};

	bool Pipe::BeginCall(CallKind a_kind)
	{
		std::scoped_lock lock(_stateMutex);
		if (_closing) return false;
		switch (a_kind) {
		case CallKind::Open:
			if (_opening || _pipe != INVALID_HANDLE_VALUE) return false;
			_opening = true;
			break;
		case CallKind::Accept:
			if (_accepting || _connected || _pipe == INVALID_HANDLE_VALUE) return false;
			_accepting = true;
			break;
		case CallKind::Read:
			if (_readerActive || !_connected) return false;
			_readerActive = true;
			break;
		case CallKind::Write:
			if (!_connected) return false;
			break;
		}
		++_activeCalls;
		return true;
	}

	void Pipe::EndCall(CallKind a_kind)
	{
		bool idle = false;
		{
			std::scoped_lock lock(_stateMutex);
			if (a_kind == CallKind::Open) _opening = false;
			if (a_kind == CallKind::Accept) _accepting = false;
			if (a_kind == CallKind::Read) _readerActive = false;
			if (_activeCalls > 0) --_activeCalls;
			idle = _activeCalls == 0;
		}
		if (idle) _idle.notify_all();
	}

	void Pipe::PrepareForOpen()
	{
		std::scoped_lock lifecycleLock(_lifecycleMutex);
		CloseLocked();
		{
			std::scoped_lock stateLock(_stateMutex);
			_closing = false;
		}
		std::scoped_lock errorLock(_errorMutex);
		_lastError.clear();
	}

	void Pipe::SetError(const char* a_where, DWORD a_code)
	{
		std::scoped_lock lock(_errorMutex);
		_lastError = std::string(a_where) + " failed (" + std::to_string(a_code) + ")";
	}

	bool Pipe::IsCreated() const
	{
		std::scoped_lock lock(_stateMutex);
		return !_closing && _pipe != INVALID_HANDLE_VALUE;
	}

	bool Pipe::IsOpen() const
	{
		std::scoped_lock lock(_stateMutex);
		return !_closing && _connected;
	}

	std::optional<std::uint32_t> Pipe::ClientProcessId()
	{
		DWORD pid = 0;
		DWORD error = ERROR_SUCCESS;
		{
			std::scoped_lock lock(_stateMutex);
			if (_closing || !_connected) return std::nullopt;
			if (!::GetNamedPipeClientProcessId(_pipe, &pid)) error = ::GetLastError();
		}
		if (error != ERROR_SUCCESS) {
			SetError("GetNamedPipeClientProcessId", error);
			return std::nullopt;
		}
		return pid;
	}

	std::optional<std::uint32_t> Pipe::ServerProcessId()
	{
		DWORD pid = 0;
		DWORD error = ERROR_SUCCESS;
		{
			std::scoped_lock lock(_stateMutex);
			if (_closing || !_connected) return std::nullopt;
			if (!::GetNamedPipeServerProcessId(_pipe, &pid)) error = ::GetLastError();
		}
		if (error != ERROR_SUCCESS) {
			SetError("GetNamedPipeServerProcessId", error);
			return std::nullopt;
		}
		return pid;
	}

	bool Pipe::IsClosing() const
	{
		std::scoped_lock lock(_stateMutex);
		return _closing;
	}

	std::string Pipe::LastErrorText() const
	{
		std::scoped_lock lock(_errorMutex);
		return _lastError;
	}

	bool Pipe::PublishOpenHandles(HANDLE a_pipe, HANDLE a_readEvent,
		HANDLE a_writeEvent, bool a_connected)
	{
		{
			std::scoped_lock lock(_stateMutex);
			if (!_closing) {
				_pipe = a_pipe;
				_readEvent = a_readEvent;
				_writeEvent = a_writeEvent;
				_connected = a_connected;
				return true;
			}
		}
		::CloseHandle(a_readEvent);
		::CloseHandle(a_writeEvent);
		::CloseHandle(a_pipe);
		return false;
	}

	bool Pipe::CreateServer(const std::wstring& a_name)
	{
		if (!BeginCall(CallKind::Open)) return false;
		CallGuard call(*this, CallKind::Open);
		const auto fail = [&] {
			call.Release();
			Close();
			return false;
		};

		PSECURITY_DESCRIPTOR sd = nullptr;
		if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
				kOwnerOnlySddl, SDDL_REVISION_1, &sd, nullptr)) {
			SetError("ConvertStringSecurityDescriptor", ::GetLastError());
			return fail();
		}
		SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };

		const auto path = L"\\\\.\\pipe\\" + a_name;
		const HANDLE pipe = ::CreateNamedPipeW(path.c_str(),
			PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
			1, 1 << 20, 1 << 20, 0, &sa);
		::LocalFree(sd);
		if (pipe == INVALID_HANDLE_VALUE) {
			SetError("CreateNamedPipe", ::GetLastError());
			return fail();
		}

		const HANDLE readEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		const HANDLE writeEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!readEvent || !writeEvent) {
			SetError("CreateEvent", ::GetLastError());
			if (readEvent) ::CloseHandle(readEvent);
			if (writeEvent) ::CloseHandle(writeEvent);
			::CloseHandle(pipe);
			return fail();
		}

		if (!PublishOpenHandles(pipe, readEvent, writeEvent, false)) return fail();
		return true;
	}

	bool Pipe::WaitForClient(std::uint32_t a_timeoutMs)
	{
		if (!BeginCall(CallKind::Accept)) return false;
		CallGuard call(*this, CallKind::Accept);
		const auto fail = [&] {
			call.Release();
			Close();
			return false;
		};

		OVERLAPPED ov{};
		HANDLE pipe = INVALID_HANDLE_VALUE;
		bool connected = false;
		bool pending = false;
		DWORD error = ERROR_SUCCESS;
		bool ready = false;
		{
			std::scoped_lock lock(_stateMutex);
			if (!_closing && _pipe != INVALID_HANDLE_VALUE) {
				ready = true;
				pipe = _pipe;
				ov.hEvent = _readEvent;
				::ResetEvent(_readEvent);
				if (::ConnectNamedPipe(pipe, &ov)) {
					connected = true;
				} else {
					error = ::GetLastError();
					pending = error == ERROR_IO_PENDING;
					connected = error == ERROR_PIPE_CONNECTED;
				}
			}
		}
		if (!ready) return fail();
		if (!connected && !pending) {
			SetError("ConnectNamedPipe", error);
			return fail();
		}
		if (pending) {
			const auto wait = ::WaitForSingleObject(ov.hEvent, a_timeoutMs);
			if (wait != WAIT_OBJECT_0) {
				const DWORD waitError = wait == WAIT_TIMEOUT ? WAIT_TIMEOUT :
					wait == WAIT_FAILED ? ::GetLastError() : ERROR_GEN_FAILURE;
				::CancelIoEx(pipe, &ov);
				DWORD ignored = 0;
				::GetOverlappedResult(pipe, &ov, &ignored, TRUE);
				if (!IsClosing()) SetError("ConnectNamedPipe wait", waitError);
				return fail();
			}
			DWORD ignored = 0;
			if (!::GetOverlappedResult(pipe, &ov, &ignored, FALSE)) {
				const auto resultError = ::GetLastError();
				if (!IsClosing()) SetError("ConnectNamedPipe result", resultError);
				return fail();
			}
		}

		bool published = false;
		{
			std::scoped_lock lock(_stateMutex);
			if (!_closing && _pipe == pipe) {
				_connected = true;
				published = true;
			}
		}
		if (!published) return fail();
		return true;
	}

	bool Pipe::CreateServerAndWait(const std::wstring& a_name, std::uint32_t a_timeoutMs)
	{
		return CreateServer(a_name) && WaitForClient(a_timeoutMs);
	}
	bool Pipe::Connect(const std::wstring& a_name, std::uint32_t a_timeoutMs)
	{
		if (!BeginCall(CallKind::Open)) return false;
		CallGuard call(*this, CallKind::Open);
		const auto fail = [&] {
			call.Release();
			Close();
			return false;
		};

		const auto path = L"\\\\.\\pipe\\" + a_name;
		const auto deadline = ::GetTickCount64() + a_timeoutMs;
		HANDLE pipe = INVALID_HANDLE_VALUE;
		for (;;) {
			if (IsClosing()) return fail();
			pipe = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
				nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
			if (pipe != INVALID_HANDLE_VALUE) break;
			const auto error = ::GetLastError();
			if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) {
				SetError("CreateFile(pipe)", error);
				return fail();
			}
			if (::GetTickCount64() >= deadline) {
				SetError("pipe connect (timeout)", error);
				return fail();
			}
			::Sleep(50);
		}

		const HANDLE readEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		const HANDLE writeEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!readEvent || !writeEvent) {
			SetError("CreateEvent", ::GetLastError());
			if (readEvent) ::CloseHandle(readEvent);
			if (writeEvent) ::CloseHandle(writeEvent);
			::CloseHandle(pipe);
			return fail();
		}

		if (!PublishOpenHandles(pipe, readEvent, writeEvent, true)) return fail();
		return true;
	}

	bool Pipe::ReadExact(std::uint8_t* a_buffer, std::uint32_t a_bytes,
		std::uint64_t a_deadline)
	{
		std::uint32_t done = 0;
		while (done < a_bytes) {
			DWORD waitMs = INFINITE;
			if (a_deadline != 0) {
				const auto now = ::GetTickCount64();
				if (now >= a_deadline) {
					SetError("ReadFile wait", WAIT_TIMEOUT);
					return false;
				}
				waitMs = static_cast<DWORD>(std::min<std::uint64_t>(
					a_deadline - now, MAXDWORD - 1));
			}

			OVERLAPPED ov{};
			HANDLE pipe = INVALID_HANDLE_VALUE;
			DWORD got = 0;
			DWORD error = ERROR_SUCCESS;
			{
				// Close takes this same lock before cancellation, so every I/O is
				// either already pending and cancellable or observes _closing.
				std::scoped_lock lock(_stateMutex);
				if (_closing || _pipe == INVALID_HANDLE_VALUE) return false;
				pipe = _pipe;
				ov.hEvent = _readEvent;
				::ResetEvent(_readEvent);
				if (!::ReadFile(pipe, a_buffer + done, a_bytes - done, &got, &ov)) {
					error = ::GetLastError();
				}
			}
			if (error != ERROR_SUCCESS && error != ERROR_IO_PENDING) {
				if (!IsClosing()) SetError("ReadFile", error);
				return false;
			}
			if (error == ERROR_IO_PENDING) {
				const auto wait = ::WaitForSingleObject(ov.hEvent, waitMs);
				if (wait != WAIT_OBJECT_0) {
					const DWORD waitError = wait == WAIT_TIMEOUT ? WAIT_TIMEOUT :
						wait == WAIT_FAILED ? ::GetLastError() : ERROR_GEN_FAILURE;
					::CancelIoEx(pipe, &ov);
					DWORD ignored = 0;
					::GetOverlappedResult(pipe, &ov, &ignored, TRUE);
					if (!IsClosing()) SetError("ReadFile wait", waitError);
					return false;
				}
				if (!::GetOverlappedResult(pipe, &ov, &got, FALSE)) {
					const auto resultError = ::GetLastError();
					if (!IsClosing()) SetError("ReadFile overlapped", resultError);
					return false;
				}
			}
			if (got == 0) {
				if (!IsClosing()) SetError("ReadFile", ERROR_BROKEN_PIPE);
				return false;
			}
			done += got;
		}
		return true;
	}

	bool Pipe::ReadMessage(std::string& a_payload, std::uint32_t a_timeoutMs)
	{
		if (!BeginCall(CallKind::Read)) return false;
		CallGuard call(*this, CallKind::Read);

		const auto deadline = a_timeoutMs == INFINITE ? 0 :
			::GetTickCount64() + a_timeoutMs;
		std::uint8_t header[4]{};
		if (!ReadExact(header, sizeof(header), deadline)) return false;
		const std::uint32_t length = header[0] | (header[1] << 8) |
			(header[2] << 16) | (static_cast<std::uint32_t>(header[3]) << 24);
		if (length == 0 || length > kMaxMessageBytes) {
			SetError("frame length", length);
			return false;
		}
		a_payload.resize(length);
		return ReadExact(
			reinterpret_cast<std::uint8_t*>(a_payload.data()), length, deadline);
	}

	bool Pipe::WriteMessage(const std::string& a_payload)
	{
		if (a_payload.empty() || a_payload.size() > kMaxMessageBytes) {
			SetError("payload size", static_cast<DWORD>(a_payload.size()));
			return false;
		}
		if (!BeginCall(CallKind::Write)) return false;
		CallGuard call(*this, CallKind::Write);

		const auto length = static_cast<std::uint32_t>(a_payload.size());
		std::vector<std::uint8_t> buffer(4 + a_payload.size());
		buffer[0] = static_cast<std::uint8_t>(length & 0xFF);
		buffer[1] = static_cast<std::uint8_t>((length >> 8) & 0xFF);
		buffer[2] = static_cast<std::uint8_t>((length >> 16) & 0xFF);
		buffer[3] = static_cast<std::uint8_t>((length >> 24) & 0xFF);
		std::memcpy(buffer.data() + 4, a_payload.data(), a_payload.size());

		std::scoped_lock writeLock(_writeMutex);
		std::uint32_t done = 0;
		while (done < buffer.size()) {
			OVERLAPPED ov{};
			HANDLE pipe = INVALID_HANDLE_VALUE;
			DWORD wrote = 0;
			DWORD error = ERROR_SUCCESS;
			{
				std::scoped_lock stateLock(_stateMutex);
				if (_closing || _pipe == INVALID_HANDLE_VALUE) return false;
				pipe = _pipe;
				ov.hEvent = _writeEvent;
				::ResetEvent(_writeEvent);
				if (!::WriteFile(pipe, buffer.data() + done,
						static_cast<DWORD>(buffer.size() - done), &wrote, &ov)) {
					error = ::GetLastError();
				}
			}
			if (error != ERROR_SUCCESS && error != ERROR_IO_PENDING) {
				if (!IsClosing()) SetError("WriteFile", error);
				return false;
			}
			if (error == ERROR_IO_PENDING &&
				!::GetOverlappedResult(pipe, &ov, &wrote, TRUE)) {
				const auto resultError = ::GetLastError();
				if (!IsClosing()) SetError("WriteFile overlapped", resultError);
				return false;
			}
			if (wrote == 0) {
				if (!IsClosing()) SetError("WriteFile", ERROR_WRITE_FAULT);
				return false;
			}
			done += wrote;
		}
		return true;
	}

	void Pipe::CloseLocked()
	{
		HANDLE pipe = INVALID_HANDLE_VALUE;
		HANDLE readEvent = nullptr;
		HANDLE writeEvent = nullptr;
		{
			std::unique_lock stateLock(_stateMutex);
			_closing = true;
			if (_pipe != INVALID_HANDLE_VALUE) {
				// I/O is issued under _stateMutex, so cancellation cannot miss an
				// operation that passed its close check but has not started yet.
				::CancelIoEx(_pipe, nullptr);
			}
			// A timeout cannot safely free OVERLAPPED handles that a live call still
			// references. Process teardown never enters this path: its owners have
			// process lifetime, while normal recovery keeps the I/O threads alive.
			_idle.wait(stateLock, [this] { return _activeCalls == 0; });
			pipe = std::exchange(_pipe, INVALID_HANDLE_VALUE);
			readEvent = std::exchange(_readEvent, nullptr);
			writeEvent = std::exchange(_writeEvent, nullptr);
			_opening = false;
			_accepting = false;
			_readerActive = false;
			_connected = false;
		}
		if (pipe != INVALID_HANDLE_VALUE) ::CloseHandle(pipe);
		if (readEvent) ::CloseHandle(readEvent);
		if (writeEvent) ::CloseHandle(writeEvent);
	}

	void Pipe::Close()
	{
		std::scoped_lock lifecycleLock(_lifecycleMutex);
		CloseLocked();
	}
}
