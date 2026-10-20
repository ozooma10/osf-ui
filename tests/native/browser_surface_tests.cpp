#include "BrowserSurface.h"
#include "check.h"
#include <iostream>

int main()
{
    using namespace osfui::browser;
    CHECK(BrowserDestination(" example.com/watch?v=1 ") == "https://example.com/watch?v=1");
    CHECK(BrowserDestination("//example.com") == "https://example.com");
    CHECK(BrowserDestination("example.com:8443/music") == "https://example.com:8443/music");
    CHECK(BrowserDestination("http://example.com") == "http://example.com");
    CHECK(BrowserDestination("[2606:4700:4700::1111]/") == "https://[2606:4700:4700::1111]/");
    CHECK(BrowserDestination("space music") == "https://duckduckgo.com/?q=space%20music");
    CHECK(BrowserDestination("a+b & c", "google") == "https://www.google.com/search?q=a%2Bb%20%26%20c");
    CHECK(BrowserDestination("music", "bing") == "https://www.bing.com/search?q=music");
    CHECK(BrowserDestination("caf\xC3\xA9") == "https://duckduckgo.com/?q=caf%C3%A9");
    CHECK(BrowserDestination("https://example.com/caf\xC3\xA9") == "https://example.com/caf%C3%A9");
    for (const auto* input : {"", "  ", "http://localhost", "127.0.0.1", "[::1]", "192.168.1.1:80",
        "https://osfui.example/shared/osfui.js", "javascript:alert(1)", "data:text/html,hi", "file:///C:/a",
        "steam://run/1", "https://user:pass@example.com", "https://example.com\\@127.0.0.1",
        "https://example.com/a b", "https://b\xC3\xBC""cher.de", "foo\nbar"})
        CHECK(!BrowserDestination(input));
    CHECK(!BrowserDestination("music", "https://localhost/"));
    CHECK(!BrowserDestination(std::string(8193, 'a')));

    const auto chrome=BrowserPanelRect(1920,1080,56,true,false,BrowserPanel::Chrome);
    const auto content=BrowserPanelRect(1920,1080,56,true,false,BrowserPanel::Content);
    CHECK(chrome.height == 56 && content.y == 56 && content.height == 1024);
    CHECK(chrome.Contains(1919,55) && !chrome.Contains(1920,55) && !chrome.Contains(0,56));
    CHECK(content.Contains(0,56) && content.Contains(1919,1079) && !content.Contains(0,1080));
    const auto world=BrowserPanelRect(1920,1080,56,false,false,BrowserPanel::Content);
    CHECK(world.y == 0 && world.height == 1080);
    CHECK(BrowserPanelRect(1920,1080,56,false,false,BrowserPanel::Chrome).height == 0);
    CHECK(BrowserPanelRect(1920,1080,56,false,true,BrowserPanel::Chrome).height == 1080);
    CHECK(BrowserPanelRect(1920,1080,56,true,true,BrowserPanel::Content).height == 0);

    BrowserPointerRouter pointer;
    const auto route=[&](int x,int y,bool button=false,unsigned which=0,bool down=false) {
        return pointer.Route(x,y,button,which,down,chrome,content);
    };
    CHECK(route(-1,20) == BrowserPanel::None);
    CHECK(route(5,55) == BrowserPanel::Chrome);
    CHECK(route(5,56,true,0,true) == BrowserPanel::Content);
    CHECK(route(5,20) == BrowserPanel::Content); // Dragging across chrome cannot give chrome the click.
    CHECK(route(5,20,true,1,true) == BrowserPanel::Content);
    CHECK(route(-20,-20,true,0,false) == BrowserPanel::Content);
    CHECK(route(5,20) == BrowserPanel::Content); // Second button still owns capture.
    CHECK(route(-20,-20,true,1,false) == BrowserPanel::Content);
    CHECK(route(5,20) == BrowserPanel::Chrome);
    CHECK(route(5,20,true,0,true) == BrowserPanel::Chrome);
    pointer.Reset(); // Focus/layout changes release capture.
    CHECK(route(5,60) == BrowserPanel::Content);
    std::cout << "browser_surface_tests: " << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures;
}
