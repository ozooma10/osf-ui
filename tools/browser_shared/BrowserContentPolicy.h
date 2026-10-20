#pragma once

#include "BrowserBackend.h"
#include "BrowserRemoteUri.h"

#include <filesystem>

namespace osfui::browser
{
	// The host selects this role before creating a controller/browser and never
	// changes it. URLs cannot grant a remote document access to local assets/IPC.
	[[nodiscard]] inline bool IsAllowedContentDocument(bool remote, std::wstring_view uri,
		std::wstring_view mod, std::wstring_view view, bool child = false)
	{
		if (remote) return IsAllowedRemoteNavigationUri(uri) ||
			EqualsAsciiCaseInsensitive(uri, L"about:blank") ||
			(child && EqualsAsciiCaseInsensitive(uri, L"about:srcdoc"));
		return IsTrustedViewDocumentUri(uri, kViewHost, mod, view) ||
			(child && IsAllowedBlankFrameUri(uri));
	}

	[[nodiscard]] inline bool IsAllowedContentResource(bool remote, std::wstring_view uri)
	{
		if (!remote) return IsAllowedViewResourceUri(uri, kViewHost);
		if (IsAllowedRemoteNavigationUri(uri)) return true;
		// These carry in-memory bytes, never OSF UI's virtual asset mapping.
		if (EqualsAsciiCaseInsensitive(uri.substr(0, 5), L"data:")) return true;
		return EqualsAsciiCaseInsensitive(uri.substr(0, 5), L"blob:") &&
			IsAllowedRemoteNavigationUri(uri.substr(5));
	}

	[[nodiscard]] constexpr bool CanCreateContentView(bool remoteMode, bool remoteContent, bool legacy)
	{
		return !remoteContent || (remoteMode && !legacy);
	}

	[[nodiscard]] inline std::filesystem::path RemoteProfilePath(const std::filesystem::path& localRoot, Backend backend)
	{
		return localRoot / "browser" / BackendName(backend);
	}

	[[nodiscard]] inline std::wstring WebViewBrowserArguments(bool remoteMode)
	{
		std::wstring args = L"--disable-backgrounding-occluded-windows --disable-renderer-backgrounding "
			L"--disable-features=CalculateNativeWinOcclusion,ApplyNativeOcclusionToCompositor,"
			L"WindowsScrollingPersonality,PercentBasedScrolling "
			L"--disable-quic --force-webrtc-ip-handling-policy=disable_non_proxied_udp ";
		if (remoteMode) args += L"--autoplay-policy=no-user-gesture-required";
		else args += L"--proxy-server=http://osfui-egress.invalid:9 --proxy-bypass-list=\"<-loopback>\" "
			L"--host-resolver-rules=\"MAP * ~NOTFOUND\"";
		return args;
	}
}
