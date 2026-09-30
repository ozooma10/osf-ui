#include "API/BridgeApi.h"
#include "Core/Log.h"
#include "check.h"

extern "C" void* OSFUI_RequestAPI(std::uint32_t, std::uint32_t*) noexcept;

namespace OSFUI::Log
{
    void WarnOnce(std::once_flag& flag, std::string_view message)
    {
        std::call_once(flag, [&] { REX::test::Log("WARN", std::string(message)); });
    }
    bool DebugEnabled() { return false; }
    void SetDebugLogging(bool) {}
}

namespace
{
    OSFUI::API::AcquireFn g_acquire = &OSFUI_RequestAPI;

    // Development DLLs used the same export with an incompatible API 1.0 layout.
    void* DevelopmentAPI(std::uint32_t version, std::uint32_t* actual) noexcept
    {
        const bool supported = OSFUI::API::Supports(0x00010000u, version);
        if (actual) *actual = supported ? 0x00010000u : 0;
        // This sentinel must never be exposed or dereferenced as a release IUI.
        return supported ? reinterpret_cast<void*>(1) : nullptr;
    }
}

int main()
{
    using namespace OSFUI::API;
    auto* expected = static_cast<IUI*>(&BridgeApi::Get());
    std::uint32_t actual = 0;

    CHECK(kBaseVersion == 0x00020000u);
    CHECK(OSFUI_RequestAPI(0x00020000u, &actual) == expected);
    CHECK(actual == kVersion);
    CHECK(OSFUI_RequestAPI(kBaseVersion, nullptr) == expected);
    for (const auto version : {0u, 0x00010000u, 0x0001FFFFu, kVersion + 1, 0x00030000u}) {
        actual = 0xFFFFFFFFu;
        CHECK(OSFUI_RequestAPI(version, &actual) == nullptr);
        CHECK(actual == 0);
        CHECK(OSFUI_RequestAPI(version, nullptr) == nullptr);
    }

    REX::W32::test::moduleLookup = [](const wchar_t* name) noexcept -> void* {
        return std::wstring_view(name) == L"OSFUI.dll" ? reinterpret_cast<void*>(1) : nullptr;
    };
    REX::W32::test::procLookup = [](void*, const char* name) noexcept -> void* {
        return std::string_view(name) == "OSFUI_RequestAPI" ? reinterpret_cast<void*>(g_acquire) : nullptr;
    };
    CHECK(RequestInterface(kBaseVersion, &actual) == expected);
    CHECK(actual == kVersion);
    CHECK(RequestInterface() == expected);
    Client client;
    CHECK(client.Init());
    CHECK(client.Version() == kVersion);
    CHECK(client.Has(kBaseVersion));
    CHECK(!client.Has(0x00010000u));

    // Both directions of the release/development mismatch fail acquisition.
    g_acquire = &DevelopmentAPI;
    actual = 0xFFFFFFFFu;
    CHECK(RequestInterface(kBaseVersion, &actual) == nullptr);
    CHECK(actual == 0);
    CHECK(!client.Init());
    CHECK(!client && client.Version() == 0 && client.Raw() == nullptr);
    actual = 0xFFFFFFFFu;
    CHECK(RequestInterface(0x00010000u, &actual) == nullptr);
    CHECK(actual == 0);
    CHECK(!client.Attach(expected, 0x00010000u));

    // Even an export that ignores the requested version cannot leak an old layout.
    g_acquire = [](std::uint32_t, std::uint32_t* reported) noexcept -> void* {
        if (reported) *reported = 0x00010000u;
        return reinterpret_cast<void*>(1);
    };
    actual = 0xFFFFFFFFu;
    CHECK(RequestInterface(kBaseVersion, &actual) == nullptr);
    CHECK(actual == 0);
    CHECK(RequestInterface() == nullptr);
    CHECK(!client.Init());

    // Same-major minor additions remain usable, but cannot satisfy a newer request.
    g_acquire = [](std::uint32_t, std::uint32_t* reported) noexcept -> void* {
        if (reported) *reported = kVersion + 1;
        return static_cast<IUI*>(&BridgeApi::Get());
    };
    CHECK(client.Init());
    CHECK(client.Version() == kVersion + 1 && client.Has(kVersion + 1));
    CHECK(RequestInterface(kVersion + 1, &actual) == expected);
    CHECK(actual == kVersion + 1);
    actual = 0xFFFFFFFFu;
    CHECK(RequestInterface(kVersion + 2, &actual) == nullptr);
    CHECK(actual == 0);

    std::fprintf(stderr, "api_version_tests: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}
