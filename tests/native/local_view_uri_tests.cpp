#include "../../tools/webview2_shared/Wv2LocalUri.h"

#include <cassert>
#include <iostream>

int main()
{
	using namespace osfui::wv2;
	const std::wstring host(kViewHost);
	const std::wstring modId = L"acme.widgets";
	const std::wstring viewName = L"inventory";

	// Resource checks accept only the exact shared HTTPS virtual host.
	assert(IsHttpsUriForHost(L"https://osfui.example", host));
	assert(IsHttpsUriForHost(L"HTTPS://OSFUI.EXAMPLE/acme.widgets/inventory/app.js?x=1#y", host));
	assert(!IsHttpsUriForHost(L"http://osfui.example/acme.widgets/inventory/app.js", host));
	assert(!IsHttpsUriForHost(L"https://osfui.example.evil.com/", host));
	assert(!IsHttpsUriForHost(L"https://osfui.examplehost/", host));
	assert(!IsHttpsUriForHost(L"https://osfui.example:443/", host));
	assert(!IsHttpsUriForHost(L"https://osfui.example@evil.com/", host));
	assert(!IsHttpsUriForHost(L"https://evil.com/osfui.example/", host));
	assert(!IsHttpsUriForHost(L"https://osfui.example\\acme.widgets\\inventory\\index.html", host));

	// One WebView may navigate and send native messages only within its assigned
	// mod/view document subtree, despite all static files sharing one origin.
	assert(IsTrustedViewDocumentUri(
		L"https://osfui.example/acme.widgets/inventory/index.html", host, modId, viewName));
	assert(IsTrustedViewDocumentUri(
		L"https://osfui.example/acme.widgets/inventory", host, modId, viewName));
	assert(IsTrustedViewDocumentUri(
		L"https://osfui.example/acme.widgets/inventory/pages/detail.html?id=4", host, modId, viewName));
	assert(!IsTrustedViewDocumentUri(
		L"https://osfui.example/acme.widgets/inventory-old/index.html", host, modId, viewName));
	assert(!IsTrustedViewDocumentUri(
		L"https://osfui.example/acme.widgets/other/index.html", host, modId, viewName));
	assert(!IsTrustedViewDocumentUri(
		L"https://osfui.example/other.mod/inventory/index.html", host, modId, viewName));
	assert(!IsTrustedViewDocumentUri(
		L"https://osfui.example/acme.widgets/inventory/../other/index.html", host, modId, viewName));
	assert(!IsTrustedViewDocumentUri(
		L"https://osfui.example/acme.widgets/inventory/%2e%2e/other/index.html", host, modId, viewName));
	assert(!IsTrustedViewDocumentUri(
		L"https://osfui.example/shared/osfui.js", host, modId, viewName));

	// Static subresources deliberately share the root mapping. The network guard
	// still rejects every other host and unsafe virtual path.
	assert(IsAllowedViewResourceUri(L"https://osfui.example/shared/osfui.css", host));
	assert(IsAllowedViewResourceUri(L"https://osfui.example/acme.widgets/inventory/app.js", host));
	assert(IsAllowedViewResourceUri(L"https://osfui.example/other.mod/other/app.js", host));
	assert(!IsAllowedViewResourceUri(L"https://example.org/app.js", host));
	assert(!IsAllowedViewResourceUri(L"https://osfui.example/acme.widgets/../other/app.js", host));

	assert(IsAllowedBlankFrameUri(L"about:blank"));
	assert(IsAllowedBlankFrameUri(L"ABOUT:SRCDOC"));
	assert(!IsAllowedBlankFrameUri(L"data:text/html,hello"));

	std::cout << "WebView2 origin policy tests passed\n";
}
