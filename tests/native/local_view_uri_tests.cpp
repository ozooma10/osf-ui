// Default-deny egress policy's single decision point: everything the host's
// WebResourceRequested filter does NOT 403 must be the view virtual host or a
// path under it. A wrong "true" here is a network egress hole, so the lookalike
// cases matter more than the happy paths.

#include "../../tools/webview2_shared/Wv2LocalUri.h"

#include <cassert>
#include <iostream>

int main()
{
	using osfui::wv2::IsLocalViewUri;
	const std::wstring host = L"osfui.local";

	// Local: the host itself and any path under it, both schemes.
	assert(IsLocalViewUri(L"https://osfui.local", host));
	assert(IsLocalViewUri(L"https://osfui.local/", host));
	assert(IsLocalViewUri(L"https://osfui.local/osfui/settings/index.html", host));
	assert(IsLocalViewUri(L"http://osfui.local/x", host));
	// Scheme and host are case-insensitive.
	assert(IsLocalViewUri(L"HTTPS://OSFUI.LOCAL/Views/App.JS", host));

	// Lookalike hosts that merely START with the virtual host must be remote:
	// the boundary is base + '/'.
	assert(!IsLocalViewUri(L"https://osfui.local.evil.com/", host));
	assert(!IsLocalViewUri(L"https://osfui.localhost/", host));
	// A port or userinfo re-scopes the authority.
	assert(!IsLocalViewUri(L"https://osfui.local:8080/", host));
	assert(!IsLocalViewUri(L"https://osfui.local@evil.com/", host));
	// The virtual host in a non-authority position.
	assert(!IsLocalViewUri(L"https://evil.com/osfui.local/", host));
	assert(!IsLocalViewUri(L"https://evil.com/?d=https://osfui.local/", host));
	// Other schemes are not this filter's business (and must not match).
	assert(!IsLocalViewUri(L"wss://osfui.local/", host));
	assert(!IsLocalViewUri(L"file://osfui.local/", host));
	assert(!IsLocalViewUri(L"", host));
	assert(!IsLocalViewUri(L"https://", host));
	assert(!IsLocalViewUri(L"https://evil.invalid/", host));

	std::cout << "local view uri tests passed\n";
}
