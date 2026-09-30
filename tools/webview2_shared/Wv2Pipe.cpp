#include "Wv2Pipe.h"

#include "Wv2Protocol.h"

#include <algorithm>
#include <utility>

#include <sddl.h>

namespace osfui::wv2
{
	namespace
	{
		constexpr wchar_t kOwnerOnlySddl[] = L"D:P(A;;GA;;;OW)";
	}

	class Pipe::CallGuard
	{
	public:
		CallGuard(Pipe& a_owner, CallKind a_kind) :
			m_owner(&a_owner), m_kind(a_kind)
		{}
		~CallGuard() { Release(); }
		CallGuard(const CallGuard&) = delete;
		CallGuard& operator=(const CallGuard&) = delete;

		void Release()
		{
			if (m_owner) {
				m_owner->EndCall(m_kind);
				m_owner = nullptr;
			}
		}

	private:
		Pipe* m_owner;
		CallKind m_kind;
	};

	bool Pipe::BeginCall(CallKind a_kind)
	{
		std::scoped_lock lock(m_stateMutex);
		if (m_closing) return false;
		switch (a_kind) {
		case CallKind::Open:
			if (m_opening || m_pipe != INVALID_HANDLE_VALUE) return false;
			m_opening = true;
			break;
		case CallKind::Accept:
			if (m_accepting || m_connected || m_pipe == INVALID_HANDLE_VALUE) return false;
			m_accepting = true;
			break;
		case CallKind::Read:
			if (m_readerActive || !m_connected) return false;
			m_readerActive = true;
			break;
		case CallKind::Write:
			if (!m_connected) return false;
			break;
		}
		++m_activeCalls;
		return true;
	}

	void Pipe::EndCall(CallKind a_kind)
	{
		bool idle = false;
		{
			std::scoped_lock lock(m_stateMutex);
			if (a_kind == CallKind::Open) m_opening = false;
			if (a_kind == CallKind::Accept) m_accepting = false;
			if (a_kind == CallKind::Read) m_readerActive = false;
			if (m_activeCalls > 0) --m_activeCalls;
			idle = m_activeCalls == 0;
		}
		if (idle) m_idle.notify_all();
	}

	void Pipe::PrepareForOpen()
	{
		std::scoped_lock lifecycleLock(m_lifecycleMutex);
		CloseLocked();
		{
			std::scoped_lock stateLock(m_stateMutex);
			m_closing = false;
		}
		std::scoped_lock errorLock(m_errorMutex);
		m_lastError.clear();
	}

	void Pipe::SetError(const char* a_where, DWORD a_code)
	{
		SetErrorText(std::string(a_where) + " failed (" + std::to_string(a_code) + ")");
	}

	void Pipe::SetErrorText(std::string a_text)
	{
		std::scoped_lock lock(m_errorMutex);
		m_lastError = std::move(a_text);
	}

	bool Pipe::IsCreated() const
	{
		std::scoped_lock lock(m_stateMutex);
		return !m_closing && m_pipe != INVALID_HANDLE_VALUE;
	}

	bool Pipe::IsOpen() const
	{
		std::scoped_lock lock(m_stateMutex);
		return !m_closing && m_connected;
	}

	std::optional<std::uint32_t> Pipe::PeerProcessId(BOOL(WINAPI* a_query)(HANDLE, PULONG), const char* a_what)
	{
		ULONG pid = 0;
		DWORD error = ERROR_SUCCESS;
		{
			std::scoped_lock lock(m_stateMutex);
			if (m_closing || !m_connected) return std::nullopt;
			if (!a_query(m_pipe, &pid)) error = ::GetLastError();
		}
		if (error != ERROR_SUCCESS) {
			SetError(a_what, error);
			return std::nullopt;
		}
		return pid;
	}

	std::optional<std::uint32_t> Pipe::ClientProcessId()
	{
		return PeerProcessId(&::GetNamedPipeClientProcessId, "GetNamedPipeClientProcessId");
	}

	std::optional<std::uint32_t> Pipe::ServerProcessId()
	{
		return PeerProcessId(&::GetNamedPipeServerProcessId, "GetNamedPipeServerProcessId");
	}

	bool Pipe::IsClosing() const
	{
		std::scoped_lock lock(m_stateMutex);
		return m_closing;
	}

	std::string Pipe::LastErrorText() const
	{
		std::scoped_lock lock(m_errorMutex);
		return m_lastError;
	}

	bool Pipe::PublishPipe(HANDLE a_pipe, bool a_connected)
	{
		const HANDLE readEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		const HANDLE writeEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (readEvent && writeEvent) {
			std::scoped_lock lock(m_stateMutex);
			if (!m_closing) {
				m_pipe = a_pipe;
				m_readEvent = readEvent;
				m_writeEvent = writeEvent;
				m_connected = a_connected;
				return true;
			}
		} else {
			SetError("CreateEvent", ::GetLastError());
		}
		if (readEvent) ::CloseHandle(readEvent);
		if (writeEvent) ::CloseHandle(writeEvent);
		::CloseHandle(a_pipe);
		return false;
	}

	DWORD Pipe::WaitOverlapped(HANDLE a_pipe, OVERLAPPED& a_ov, DWORD a_waitMs)
	{
		const auto wait = ::WaitForSingleObject(a_ov.hEvent, a_waitMs);
		if (wait == WAIT_OBJECT_0) return ERROR_SUCCESS;
		const DWORD error = wait == WAIT_TIMEOUT ? WAIT_TIMEOUT :
			wait == WAIT_FAILED ? ::GetLastError() : ERROR_GEN_FAILURE;
		::CancelIoEx(a_pipe, &a_ov);
		DWORD ignored = 0;
		::GetOverlappedResult(a_pipe, &a_ov, &ignored, TRUE);
		return error;
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

		if (!PublishPipe(pipe, false)) return fail();
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
			std::scoped_lock lock(m_stateMutex);
			if (!m_closing && m_pipe != INVALID_HANDLE_VALUE) {
				ready = true;
				pipe = m_pipe;
				ov.hEvent = m_readEvent;
				::ResetEvent(m_readEvent);
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
			if (const auto waitError = WaitOverlapped(pipe, ov, a_timeoutMs); waitError != ERROR_SUCCESS) {
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
			std::scoped_lock lock(m_stateMutex);
			if (!m_closing && m_pipe == pipe) {
				m_connected = true;
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

		if (!PublishPipe(pipe, true)) return fail();
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
				std::scoped_lock lock(m_stateMutex);
				if (m_closing || m_pipe == INVALID_HANDLE_VALUE) return false;
				pipe = m_pipe;
				ov.hEvent = m_readEvent;
				::ResetEvent(m_readEvent);
				if (!::ReadFile(pipe, a_buffer + done, a_bytes - done, &got, &ov)) {
					error = ::GetLastError();
				}
			}
			if (error != ERROR_SUCCESS && error != ERROR_IO_PENDING) {
				if (!IsClosing()) SetError("ReadFile", error);
				return false;
			}
			if (error == ERROR_IO_PENDING) {
				if (const auto waitError = WaitOverlapped(pipe, ov, waitMs); waitError != ERROR_SUCCESS) {
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
			SetErrorText("invalid frame length " + std::to_string(length));
			return false;
		}
		a_payload.resize(length);
		return ReadExact(
			reinterpret_cast<std::uint8_t*>(a_payload.data()), length, deadline);
	}

	Pipe::WriteResult Pipe::WriteMessage(const std::string& a_payload)
	{
		if (a_payload.empty() || a_payload.size() > kMaxMessageBytes) {
			SetErrorText("invalid payload size " + std::to_string(a_payload.size()));
			return WriteResult::InvalidPayload;
		}
		if (!BeginCall(CallKind::Write)) return WriteResult::Disconnected;
		CallGuard call(*this, CallKind::Write);

		const auto length = static_cast<std::uint32_t>(a_payload.size());
		const std::uint8_t header[4]{
			static_cast<std::uint8_t>(length & 0xFF),
			static_cast<std::uint8_t>((length >> 8) & 0xFF),
			static_cast<std::uint8_t>((length >> 16) & 0xFF),
			static_cast<std::uint8_t>((length >> 24) & 0xFF),
		};
		// The write lock keeps header and payload contiguous in the byte stream.
		std::scoped_lock writeLock(m_writeMutex);
		const auto result = WriteAll(header, sizeof(header));
		if (result != WriteResult::Written) return result;
		return WriteAll(reinterpret_cast<const std::uint8_t*>(a_payload.data()), length);
	}

	Pipe::WriteResult Pipe::WriteAll(const std::uint8_t* a_data, std::uint32_t a_bytes)
	{
		std::uint32_t done = 0;
		while (done < a_bytes) {
			OVERLAPPED ov{};
			HANDLE pipe = INVALID_HANDLE_VALUE;
			DWORD wrote = 0;
			DWORD error = ERROR_SUCCESS;
			{
				std::scoped_lock stateLock(m_stateMutex);
				if (m_closing || m_pipe == INVALID_HANDLE_VALUE) return WriteResult::Disconnected;
				pipe = m_pipe;
				ov.hEvent = m_writeEvent;
				::ResetEvent(m_writeEvent);
				if (!::WriteFile(pipe, a_data + done, a_bytes - done, &wrote, &ov)) {
					error = ::GetLastError();
				}
			}
			if (error != ERROR_SUCCESS && error != ERROR_IO_PENDING) {
				if (!IsClosing()) SetError("WriteFile", error);
				return WriteResult::Disconnected;
			}
			if (error == ERROR_IO_PENDING &&
				!::GetOverlappedResult(pipe, &ov, &wrote, TRUE)) {
				const auto resultError = ::GetLastError();
				if (!IsClosing()) SetError("WriteFile overlapped", resultError);
				return WriteResult::Disconnected;
			}
			if (wrote == 0) {
				if (!IsClosing()) SetError("WriteFile", ERROR_WRITE_FAULT);
				return WriteResult::Disconnected;
			}
			done += wrote;
		}
		return WriteResult::Written;
	}

	void Pipe::CloseLocked()
	{
		HANDLE pipe = INVALID_HANDLE_VALUE;
		HANDLE readEvent = nullptr;
		HANDLE writeEvent = nullptr;
		{
			std::unique_lock stateLock(m_stateMutex);
			m_closing = true;
			if (m_pipe != INVALID_HANDLE_VALUE) {
				::CancelIoEx(m_pipe, nullptr);
			}
			m_idle.wait(stateLock, [this] { return m_activeCalls == 0; });
			pipe = std::exchange(m_pipe, INVALID_HANDLE_VALUE);
			readEvent = std::exchange(m_readEvent, nullptr);
			writeEvent = std::exchange(m_writeEvent, nullptr);
			m_opening = false;
			m_accepting = false;
			m_readerActive = false;
			m_connected = false;
		}
		if (pipe != INVALID_HANDLE_VALUE) ::CloseHandle(pipe);
		if (readEvent) ::CloseHandle(readEvent);
		if (writeEvent) ::CloseHandle(writeEvent);
	}

	void Pipe::Close()
	{
		std::scoped_lock lifecycleLock(m_lifecycleMutex);
		CloseLocked();
	}
}
