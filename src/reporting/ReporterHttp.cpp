#include "reporting/ReporterCore.h"

#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>

namespace OSFUI::Reporting
{
	namespace
	{
		[[nodiscard]] std::wstring ToWide(std::string_view a_text)
		{
			if (a_text.empty()) return {};
			const auto count = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
				a_text.data(), static_cast<int>(a_text.size()), nullptr, 0);
			if (count <= 0) return {};
			std::wstring out(static_cast<std::size_t>(count), L'\0');
			::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a_text.data(),
				static_cast<int>(a_text.size()), out.data(), count);
			return out;
		}
	}

	HttpResponse PostJson(std::string_view a_url, std::string_view a_body)
	{
		HttpResponse out;
		const auto hasWhitespace = std::ranges::any_of(a_url,
			[](unsigned char c) { return std::isspace(c) != 0; });
		if (a_url.empty() || a_url.size() > 2048 || hasWhitespace ||
			!a_url.starts_with("https://") || a_body.size() > 1024 * 1024) {
			out.error = "invalid HTTPS endpoint or request too large";
			return out;
		}
		const auto wideUrl = ToWide(a_url);
		if (wideUrl.empty()) {
			out.error = "endpoint is not valid UTF-8";
			return out;
		}

		URL_COMPONENTS parts{};
		parts.dwStructSize = sizeof(parts);
		parts.dwSchemeLength = static_cast<DWORD>(-1);
		parts.dwHostNameLength = static_cast<DWORD>(-1);
		parts.dwUrlPathLength = static_cast<DWORD>(-1);
		parts.dwExtraInfoLength = static_cast<DWORD>(-1);
		if (!::WinHttpCrackUrl(wideUrl.c_str(), 0, 0, &parts) ||
			parts.nScheme != INTERNET_SCHEME_HTTPS || parts.dwHostNameLength == 0) {
			out.error = "could not parse HTTPS endpoint";
			return out;
		}
		const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
		std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
		if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
		if (path.empty()) path = L"/";

		const HINTERNET session = ::WinHttpOpen(L"OSFUI/reporter",
			WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
			WINHTTP_NO_PROXY_BYPASS, 0);
		if (!session) {
			out.error = "WinHttpOpen failed";
			return out;
		}
		::WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
		const HINTERNET connection = ::WinHttpConnect(session, host.c_str(), parts.nPort, 0);
		const HINTERNET request = connection ?
			::WinHttpOpenRequest(connection, L"POST", path.c_str(), nullptr,
				WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
		if (request) {
			DWORD disable = WINHTTP_DISABLE_REDIRECTS;
			::WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disable, sizeof(disable));
		}
		if (!connection || !request) {
			out.error = "could not create HTTPS request";
		} else {
			constexpr wchar_t headers[] = L"Content-Type: application/json\r\nAccept: application/json\r\n";
			const auto size = static_cast<DWORD>(a_body.size());
			if (::WinHttpSendRequest(request, headers, static_cast<DWORD>(-1),
					const_cast<char*>(a_body.data()), size, size, 0) &&
				::WinHttpReceiveResponse(request, nullptr)) {
				DWORD status = 0;
				DWORD statusSize = sizeof(status);
				if (::WinHttpQueryHeaders(request,
						WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
						WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
						WINHTTP_NO_HEADER_INDEX)) {
					out.status = status;
				}
				constexpr std::size_t kMaxResponse = 64 * 1024;
				for (;;) {
					DWORD available = 0;
					if (!::WinHttpQueryDataAvailable(request, &available) || available == 0) break;
					if (out.body.size() + available > kMaxResponse) {
						out.error = "service response too large";
						break;
					}
					const auto old = out.body.size();
					out.body.resize(old + available);
					DWORD read = 0;
					if (!::WinHttpReadData(request, out.body.data() + old, available, &read)) {
						out.body.resize(old);
						out.error = "could not read service response";
						break;
					}
					out.body.resize(old + read);
				}
				out.transportOk = out.error.empty();
			} else {
				out.error = "HTTPS request failed (" + std::to_string(::GetLastError()) + ')';
			}
		}
		if (request) ::WinHttpCloseHandle(request);
		if (connection) ::WinHttpCloseHandle(connection);
		::WinHttpCloseHandle(session);
		return out;
	}
}
