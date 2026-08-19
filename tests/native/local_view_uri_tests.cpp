
#include "../../tools/webview2_shared/Wv2LocalUri.h"

#include <cassert>
#include <iostream>

int main()
{
	using osfui::wv2::IsLocalViewUri;
	const std::wstring host = L"osfui.example";

	// Local: the host itself and any path under it, both schemes.
	assert(IsLocalViewUri(L"https://osfui.example", host));
	assert(IsLocalViewUri(L"https://osfui.example/", host));
	assert(IsLocalViewUri(L"https://osfui.example/osfui/settings/index.html", host));
	assert(IsLocalViewUri(L"http://osfui.example/x", host));
	// Scheme and host are case-insensitive.
	assert(IsLocalViewUri(L"HTTPS://OSFUI.EXAMPLE/Views/App.JS", host));

	assert(!IsLocalViewUri(L"https://osfui.example.evil.com/", host));
	assert(!IsLocalViewUri(L"https://osfui.examplehost/", host));
	// A port or userinfo re-scopes the authority.
	assert(!IsLocalViewUri(L"https://osfui.example:8080/", host));
	assert(!IsLocalViewUri(L"https://osfui.example@evil.com/", host));
	// The virtual host in a non-authority position.
	assert(!IsLocalViewUri(L"https://evil.com/osfui.example/", host));
	assert(!IsLocalViewUri(L"https://evil.com/?d=https://osfui.example/", host));
	// Other schemes are not this filter's business (and must not match).
	assert(!IsLocalViewUri(L"wss://osfui.example/", host));
	assert(!IsLocalViewUri(L"file://osfui.example/", host));
	assert(!IsLocalViewUri(L"", host));
	assert(!IsLocalViewUri(L"https://", host));
	assert(!IsLocalViewUri(L"https://evil.invalid/", host));

	std::cout << "local view uri tests passed\n";
}
