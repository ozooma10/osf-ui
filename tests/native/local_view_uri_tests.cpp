
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
		L"https://osfui-assets.example/osfui.js", host, L"inventory"));

	// Only the three documented files are exposed by the shared asset origin.
	assert(IsSharedAssetUri(L"https://osfui-assets.example/osfui.js"));
	assert(IsSharedAssetUri(L"https://OSFUI-ASSETS.EXAMPLE/osfui.css?v=2"));
	assert(IsSharedAssetUri(L"https://osfui-assets.example/gamepadnav.js"));
	assert(!IsSharedAssetUri(L"https://osfui-assets.example/other.js"));
	assert(!IsSharedAssetUri(L"http://osfui-assets.example/osfui.js"));
	assert(IsAllowedViewResourceUri(
		L"https://osfui-assets.example/osfui.css", host));
	assert(IsAllowedViewResourceUri(
		L"https://m-abc234.example/inventory/app.js", host));
	assert(!IsAllowedViewResourceUri(L"https://example.org/app.js", host));

	assert(IsAllowedBlankFrameUri(L"about:blank"));
	assert(IsAllowedBlankFrameUri(L"ABOUT:SRCDOC"));
	assert(!IsAllowedBlankFrameUri(L"data:text/html,hello"));

	std::cout << "WebView2 origin policy tests passed\n";
}
