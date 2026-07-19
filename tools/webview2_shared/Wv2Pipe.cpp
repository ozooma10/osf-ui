#include "Wv2Pipe.h"

#include "Wv2Protocol.h"

#include <cstring>

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

	void Pipe::SetError(const char* a_where, DWORD a_code)
	{
		_lastError = std::string(a_where) + " failed (" + std::to_string(a_code) + ")";
	}

	bool Pipe::CreateServerAndWait(const std::wstring& a_name, std::uint32_t a_timeoutMs)
	{
		Close();
		_closing = false;

		PSECURITY_DESCRIPTOR sd = nullptr;
		if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
				kOwnerOnlySddl, SDDL_REVISION_1, &sd, nullptr)) {
			SetError("ConvertStringSecurityDescriptor", ::GetLastError());
			return false;
		}
		SECURITY_ATTRIBUTES sa{ sizeof(sa), sd, FALSE };

		const auto path = L"\\\\.\\pipe\\" + a_name;
		_pipe = ::CreateNamedPipeW(path.c_str(),
			PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
			1, 1 << 20, 1 << 20, 0, &sa);
		::LocalFree(sd);
		if (_pipe == INVALID_HANDLE_VALUE) {
			SetError("CreateNamedPipe", ::GetLastError());
			return false;
		}

		_readEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		_writeEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!_readEvent || !_writeEvent) {
			SetError("CreateEvent", ::GetLastError());
			Close();
			return false;
		}

		OVERLAPPED ov{};
		ov.hEvent = _readEvent;
		if (!::ConnectNamedPipe(_pipe, &ov)) {
			const auto err = ::GetLastError();
			if (err == ERROR_IO_PENDING) {
				if (::WaitForSingleObject(_readEvent, a_timeoutMs) != WAIT_OBJECT_0) {
					::CancelIoEx(_pipe, &ov);
					SetError("ConnectNamedPipe wait (no client)", WAIT_TIMEOUT);
					Close();
					return false;
				}
				DWORD ignored = 0;
				if (!::GetOverlappedResult(_pipe, &ov, &ignored, TRUE)) {
					SetError("ConnectNamedPipe result", ::GetLastError());
					Close();
					return false;
				}
			} else if (err != ERROR_PIPE_CONNECTED) {
				SetError("ConnectNamedPipe", err);
				Close();
				return false;
			}
		}
		return true;
	}

	bool Pipe::Connect(const std::wstring& a_name, std::uint32_t a_timeoutMs)
	{
		Close();
		_closing = false;

		const auto path = L"\\\\.\\pipe\\" + a_name;
		const auto deadline = ::GetTickCount64() + a_timeoutMs;
		for (;;) {
			_pipe = ::CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
				nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
			if (_pipe != INVALID_HANDLE_VALUE) {
				break;
			}
			const auto err = ::GetLastError();
			if (err != ERROR_PIPE_BUSY && err != ERROR_FILE_NOT_FOUND) {
				SetError("CreateFile(pipe)", err);
				return false;
			}
			if (::GetTickCount64() >= deadline) {
				SetError("pipe connect (timeout)", err);
				return false;
			}
			::Sleep(50);
		}

		_readEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		_writeEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
		if (!_readEvent || !_writeEvent) {
			SetError("CreateEvent", ::GetLastError());
			Close();
			return false;
		}
		return true;
	}

	bool Pipe::ReadExact(std::uint8_t* a_buffer, std::uint32_t a_bytes)
	{
		std::uint32_t done = 0;
		while (done < a_bytes) {
			if (_closing || _pipe == INVALID_HANDLE_VALUE) {
				return false;
			}
			OVERLAPPED ov{};
			ov.hEvent = _readEvent;
			DWORD got = 0;
			if (!::ReadFile(_pipe, a_buffer + done, a_bytes - done, &got, &ov)) {
				const auto err = ::GetLastError();
				if (err != ERROR_IO_PENDING) {
					SetError("ReadFile", err);
					return false;
				}
				if (!::GetOverlappedResult(_pipe, &ov, &got, TRUE)) {
					SetError("ReadFile overlapped", ::GetLastError());
					return false;
				}
			}
			if (got == 0) {
				SetError("ReadFile", ERROR_BROKEN_PIPE);
				return false;
			}
			done += got;
		}
		return true;
	}

	bool Pipe::ReadMessage(std::string& a_payload)
	{
		std::uint8_t header[4]{};
		if (!ReadExact(header, sizeof(header))) {
			return false;
		}
		const std::uint32_t length = header[0] | (header[1] << 8) |
			(header[2] << 16) | (static_cast<std::uint32_t>(header[3]) << 24);
		if (length == 0 || length > kMaxMessageBytes) {
			SetError("frame length", length);
			return false;
		}
		a_payload.resize(length);
		return ReadExact(reinterpret_cast<std::uint8_t*>(a_payload.data()), length);
	}

	bool Pipe::WriteMessage(const std::string& a_payload)
	{
		if (a_payload.empty() || a_payload.size() > kMaxMessageBytes) {
			SetError("payload size", static_cast<DWORD>(a_payload.size()));
			return false;
		}
		std::scoped_lock lock(_writeMutex);
		if (_closing || _pipe == INVALID_HANDLE_VALUE) {
			return false;
		}
		const auto length = static_cast<std::uint32_t>(a_payload.size());
		std::vector<std::uint8_t> buffer(4 + a_payload.size());
		buffer[0] = static_cast<std::uint8_t>(length & 0xFF);
		buffer[1] = static_cast<std::uint8_t>((length >> 8) & 0xFF);
		buffer[2] = static_cast<std::uint8_t>((length >> 16) & 0xFF);
		buffer[3] = static_cast<std::uint8_t>((length >> 24) & 0xFF);
		std::memcpy(buffer.data() + 4, a_payload.data(), a_payload.size());

		std::uint32_t done = 0;
		while (done < buffer.size()) {
			OVERLAPPED ov{};
			ov.hEvent = _writeEvent;
			DWORD wrote = 0;
			if (!::WriteFile(_pipe, buffer.data() + done,
					static_cast<DWORD>(buffer.size() - done), &wrote, &ov)) {
				const auto err = ::GetLastError();
				if (err != ERROR_IO_PENDING) {
					SetError("WriteFile", err);
					return false;
				}
				if (!::GetOverlappedResult(_pipe, &ov, &wrote, TRUE)) {
					SetError("WriteFile overlapped", ::GetLastError());
					return false;
				}
			}
			done += wrote;
		}
		return true;
	}

	void Pipe::Close()
	{
		_closing = true;
		if (_pipe != INVALID_HANDLE_VALUE) {
			::CancelIoEx(_pipe, nullptr);
			::CloseHandle(_pipe);
			_pipe = INVALID_HANDLE_VALUE;
		}
		if (_readEvent) {
			::CloseHandle(_readEvent);
			_readEvent = nullptr;
		}
		if (_writeEvent) {
			::CloseHandle(_writeEvent);
			_writeEvent = nullptr;
		}
	}
}
