#include "../../tools/browser_shared/BrowserContentPolicy.h"
#include "check.h"

int main()
{
	using osfui::browser::IsAllowedRemoteNavigationUri;
	const std::wstring_view allowed[] = {
		L"https://www.youtube.com/watch?v=example", L"http://example.com",
		L"HTTPS://EXAMPLE.COM./path?next=http://localhost#fragment",
		L"https://example.com:443/", L"https://example.com:8443/",
		L"https://xn--bcher-kva.de/", L"https://example.com/\u00e9",
		L"https://127.0.0.1.example.com/", L"https://osfui.example.com/",
		L"https://1.1.1.1/", L"http://8.8.8.8:80/",
		L"https://172.15.255.255/", L"https://172.32.0.0/",
		L"https://192.167.255.255/", L"https://192.169.0.0/",
		L"https://100.63.255.255/", L"https://100.128.0.0/",
		L"https://[2606:4700:4700::1111]/",
		L"https://[2001:4860:4860:0:0:0:0:8888]:443/",
		L"https://[::ffff:8.8.8.8]/", L"https://[::ffff:808:808]/",
		L"https://[64:ff9b::808:808]/",
	};
	for (const auto uri : allowed) CHECK(IsAllowedRemoteNavigationUri(uri));

	const std::wstring_view blocked[] = {
		L"", L"example.com", L"//example.com/", L"https:example.com",
		L"file:///C:/secret", L"edge://settings", L"chrome://settings", L"devtools://devtools/",
		L"view-source:https://example.com", L"steam://run/1", L"mailto:me@example.com",
		L"javascript:alert(1)", L"data:text/html,hello", L"about:blank",
		L"blob:https://example.com/id", L"ws://example.com", L"ftp://example.com",
		L"https://osfui.example/shared/osfui.js", L"http://OSFUI.EXAMPLE.:80/",
		L"https://child.osfui.example/", L"https://localhost/", L"http://LOCALHOST.:80/",
		L"http://a.localhost/", L"http://printer.local/", L"http://printer.localdomain/",
		L"http://router.home.arpa/", L"http://service.internal/", L"http://router/",
		L"http://0.0.0.0/", L"http://0.1.2.3/", L"http://127.0.0.1/", L"http://127.255.255.255/",
		L"http://10.0.0.1/", L"http://172.16.0.1/", L"http://172.31.255.255/", L"http://192.168.1.1/",
		L"http://169.254.169.254/", L"http://100.64.0.1/", L"http://100.127.255.255/",
		L"http://198.18.0.1/", L"http://198.19.255.255/", L"http://224.0.0.1/", L"http://255.255.255.255/",
		// Chromium accepts these aliases: never classify them as DNS names.
		L"http://127.1/", L"http://127.0.1/", L"http://2130706433/", L"http://0177.0.0.1/",
		L"http://0x7f000001/", L"http://0x7f.0.0.1/", L"http://127.0.0.0x1/",
		L"http://127.0.0.01/", L"http://127.1./", L"http://256.1.1.1/",
		L"http://1.2.3.4.5/", L"http://example.123/", L"http://example.0x1/",
		L"http://%31%32%37.0.0.1/", L"http://local%68ost/", L"https://osfui%2eexample/",
		L"http://\uff11\uff12\uff17.0.0.1/", L"https://b\u00fccher.de/",
		L"https://user:pass@example.com/", L"https://example.com@127.0.0.1/",
		L"https://127.0.0.1@example.com/", L"https://example.com\\@localhost/",
		L"https://example.com\\path", L" https://example.com", L"https://example.com ",
		L"https://example.com/\r\n", L"https://exam\tple.com/", L"https://example.com/\x7f",
		L"https://", L"https:///example.com/", L"https://example..com/",
		L"https://example.com../", L"https://-example.com/", L"https://example-.com/",
		L"https://example.com:/", L"https://example.com:0/", L"https://example.com:65536/",
		L"https://example.com:99999999999999999999/", L"https://example.com:abc/",
		L"https://example.com:443:80/",
		L"http://[::]/", L"http://[::1]/", L"http://[0:0:0:0:0:0:0:1]/",
		L"http://[::127.0.0.1]/", L"http://[::ffff:127.0.0.1]/", L"http://[::ffff:7f00:1]/",
		L"http://[0:0:0:0:0:ffff:c0a8:101]/", L"http://[::ffff:169.254.169.254]/",
		L"http://[64:ff9b::c0a8:101]/", L"http://[64:ff9b:1::808:808]/",
		L"http://[fc00::1]/", L"http://[fd12::1]/", L"http://[fe80::1]/",
		L"http://[fec0::1]/", L"http://[ff02::1]/", L"http://[fe80::1%25eth0]/",
		L"http://[2002:7f00:1::]/", L"http://[2002:c0a8:101::]/",
		L"http://[2001:0:1234::1]/",
		// Invalid IPv6 must not slip through the global-prefix check.
		L"https://[2606:4700::1111", L"https://2606:4700::1111/",
		L"https://[2606::1::2]/", L"https://[2606:::1]/",
		L"https://[2606:1:2:3:4:5:6]/", L"https://[2606:1:2:3:4:5:6:7:8]/",
		L"https://[2606:1:2:3:4:5:6:7::]/", L"https://[2606::zz]/",
		L"https://[2606::12345]/", L"https://[2606::1:]/",
		L"https://[2606::1]ignored/", L"https://[2606::1]:/",
		L"https://[2606::8.8.8.999]/", L"https://[2606:8.8.8.8::]/",
		L"https://[2606::8.08.8.8]/",
	};
	for (const auto uri : blocked) CHECK(!IsAllowedRemoteNavigationUri(uri));
	const std::wstring embeddedNull(L"https://example.com/\0localhost", 29);
	CHECK(!IsAllowedRemoteNavigationUri(embeddedNull));
	CHECK(!IsAllowedRemoteNavigationUri(L"https://" + std::wstring(64, L'a') + L".com/"));
	// A role is chosen by the host, never inferred from a document's URL.
	using namespace osfui::browser;
	for (const bool mode : { false, true }) {
		CHECK(CanCreateContentView(mode, false, false));
		CHECK(CanCreateContentView(mode, false, true));
		CHECK(CanCreateContentView(mode, true, false) == mode);
		CHECK(!CanCreateContentView(mode, true, true));
	}
	const auto localPage = L"https://osfui.example/test/chrome/index.html";
	const auto remotePage = L"https://example.com/watch";
	CHECK(IsAllowedContentDocument(false, localPage, L"test", L"chrome"));
	CHECK(!IsAllowedContentDocument(true, localPage, L"test", L"chrome"));
	CHECK(!IsAllowedContentDocument(false, remotePage, L"test", L"chrome"));
	CHECK(IsAllowedContentDocument(true, remotePage, L"test", L"chrome"));
	CHECK(!IsAllowedContentDocument(false, L"https://osfui.example/other/view/index.html", L"test", L"chrome"));
	CHECK(IsAllowedContentDocument(true, L"about:blank", {}, {}));
	CHECK(!IsAllowedContentDocument(false, L"about:blank", L"test", L"chrome"));
	CHECK(IsAllowedContentDocument(false, L"about:blank", L"test", L"chrome", true));
	CHECK(IsAllowedContentDocument(true, L"about:srcdoc", {}, {}, true));
	CHECK(!IsAllowedContentDocument(true, L"about:srcdoc", {}, {}));
	for (const auto uri : blocked) {
		if (uri == L"about:blank") continue;
		CHECK(!IsAllowedContentDocument(true, uri, {}, {}, true));
	}
	CHECK(IsAllowedContentResource(false, localPage));
	CHECK(!IsAllowedContentResource(true, localPage));
	CHECK(!IsAllowedContentResource(false, remotePage));
	CHECK(IsAllowedContentResource(true, remotePage));
	CHECK(IsAllowedContentResource(true, L"data:image/png;base64,AAAA"));
	CHECK(IsAllowedContentResource(true, L"blob:https://example.com/id"));
	for (const auto uri : { L"blob:https://osfui.example/id", L"blob:http://127.0.0.1/id",
		L"blob:null/id", L"file:///C:/secret", L"https://printer.local/image.png",
		L"ws://127.0.0.1/", L"https://10.0.0.1/api" }) CHECK(!IsAllowedContentResource(true, uri));
	const auto profile = std::filesystem::path("LocalAppData") / "OSFUI";
	CHECK(RemoteProfilePath(profile, Backend::WebView2) == profile / "browser" / "webview2");
	CHECK(RemoteProfilePath(profile, Backend::Cef) == profile / "browser" / "cef");
	const auto offline = WebViewBrowserArguments(false), online = WebViewBrowserArguments(true);
	CHECK(offline.find(L"--proxy-server=") != offline.npos);
	CHECK(offline.find(L"MAP * ~NOTFOUND") != offline.npos);
	CHECK(online.find(L"--proxy-server=") == online.npos);
	CHECK(online.find(L"--host-resolver-rules=") == online.npos);
	CHECK(online.find(L"--autoplay-policy=no-user-gesture-required") != online.npos);
	CHECK(online.find(L"--disable-renderer-backgrounding") != online.npos);
	CHECK(online.find(L"--disable-backgrounding-occluded-windows") != online.npos);
	std::printf("remote_uri_tests: %d checks, %d failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
