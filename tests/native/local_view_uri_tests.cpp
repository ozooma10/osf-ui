
#include "../../tools/webview2_shared/Wv2LocalUri.h"

#include <cassert>
#include <iostream>

int main()
{
	using namespace osfui::wv2;
	const std::wstring host = L"m-abc234.example";

	// Resource checks accept only exact HTTPS virtual hosts.
	assert(IsHttpsUriForHost(L"https://m-abc234.example", host));
	assert(IsHttpsUriForHost(L"HTTPS://M-ABC234.EXAMPLE/view/app.js?x=1#y", host));
	assert(!IsHttpsUriForHost(L"http://m-abc234.example/view/app.js", host));
	assert(!IsHttpsUriForHost(L"https://m-abc234.example.evil.com/", host));
	assert(!IsHttpsUriForHost(L"https://m-abc234.examplehost/", host));
	assert(!IsHttpsUriForHost(L"https://m-abc234.example:443/", host));
	assert(!IsHttpsUriForHost(L"https://m-abc234.example@evil.com/", host));
	assert(!IsHttpsUriForHost(L"https://evil.com/m-abc234.example/", host));
	assert(!IsHttpsUriForHost(L"https://m-abc234.example\\view\\index.html", host));

	// One WebView may navigate only within its assigned view-name subtree.
	assert(IsTrustedViewDocumentUri(
		L"https://m-abc234.example/inventory/index.html", host, L"inventory"));
	assert(IsTrustedViewDocumentUri(
		L"https://m-abc234.example/inventory", host, L"inventory"));
	assert(IsTrustedViewDocumentUri(
		L"https://m-abc234.example/inventory/pages/detail.html?id=4", host, L"inventory"));
	assert(!IsTrustedViewDocumentUri(
		L"https://m-abc234.example/inventory-old/index.html", host, L"inventory"));
	assert(!IsTrustedViewDocumentUri(
		L"https://m-abc234.example/other/index.html", host, L"inventory"));
	assert(!IsTrustedViewDocumentUri(
		L"https://m-abc234.example/inventory/../other/index.html", host, L"inventory"));
	assert(!IsTrustedViewDocumentUri(
		L"https://m-abc234.example/inventory/%2e%2e/other/index.html", host, L"inventory"));
	assert(!IsTrustedViewDocumentUri(
		L"https://m-abc234.example/shared/osfui.js", host, L"inventory"));

	// Shared assets are served directly from each mod's own origin.
	assert(IsSharedAssetUri(L"https://m-abc234.example/shared/osfui.js", host));
	assert(IsSharedAssetUri(L"HTTPS://M-ABC234.EXAMPLE/shared/osfui.css?v=2", host));
	assert(IsSharedAssetUri(L"https://m-abc234.example/shared/gamepadnav.js", host));
	assert(!IsSharedAssetUri(L"https://m-abc234.example/shared/other.js", host));
	assert(!IsSharedAssetUri(L"https://other.example/shared/osfui.js", host));
	assert(!IsSharedAssetUri(L"http://m-abc234.example/shared/osfui.js", host));
	assert(LegacySharedAssetRedirectUri(
		L"https://osfui-assets.example/osfui.js", host) ==
		L"https://m-abc234.example/shared/osfui.js");
	assert(LegacySharedAssetRedirectUri(
		L"HTTPS://OSFUI-ASSETS.EXAMPLE/osfui.css?v=2#theme", host) ==
		L"https://m-abc234.example/shared/osfui.css?v=2#theme");
	assert(LegacySharedAssetRedirectUri(
		L"https://osfui-assets.example/gamepadnav.js", host) ==
		L"https://m-abc234.example/shared/gamepadnav.js");
	assert(!LegacySharedAssetRedirectUri(
		L"https://osfui-assets.example/other.js", host));
	assert(!LegacySharedAssetRedirectUri(
		L"http://osfui-assets.example/osfui.css", host));
	assert(!LegacySharedAssetRedirectUri(
		L"https://other.example/osfui.css", host));
	assert(IsAllowedViewResourceUri(
		L"https://m-abc234.example/shared/osfui.css", host));
	assert(IsAllowedViewResourceUri(
		L"https://m-abc234.example/inventory/app.js", host));
	assert(!IsAllowedViewResourceUri(L"https://example.org/app.js", host));

	assert(IsAllowedBlankFrameUri(L"about:blank"));
	assert(IsAllowedBlankFrameUri(L"ABOUT:SRCDOC"));
	assert(!IsAllowedBlankFrameUri(L"data:text/html,hello"));

	std::cout << "WebView2 origin policy tests passed\n";
}
