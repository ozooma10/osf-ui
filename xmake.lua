includes("lib/commonlibsf")

set_project("OSF UI")
set_version("2.1.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

add_requires("nlohmann_json 3.11.3")

option("browser_backend")
    set_default("webview2")
    set_values("webview2", "cef")
    set_showmenu(true)
    set_description("Browser edition: webview2 (recommended default), cef (bundled-runtime alternative)")
option_end()

target("osfui-cef-host")
    set_kind("phony")
    set_default(false)
    add_packages("nlohmann_json")
    on_build(function(target)
        local json = target:pkg("nlohmann_json")
        os.execv("pwsh", {"-NoProfile", "-File", path.join(os.projectdir(), "tools", "build-cef-host.ps1"),
            "-JsonInclude", path.join(json:installdir(), "include")})
    end)

-- The mirrored host is self-contained: static CRT and WebView2 loader.
target("osfui-browser-broker")
    set_kind("binary")
    set_basename("osfui_browser_broker")
    set_default(false)
    set_runtimes("MT")
    add_files("tools/browser_broker/main.cpp", "tools/browser_shared/BrowserBrokerLaunch.cpp")
    add_includedirs("tools/browser_shared")
    add_syslinks("user32", "shell32", "ole32", "oleaut32", "uuid", "advapi32", "taskschd", "comsuppw")
    add_ldflags("/SUBSYSTEM:WINDOWS", {force=true})

target("osfui-webview2-host")
    set_kind("binary")
    set_basename("osfui_webview2_host")
    set_default(false)
    set_runtimes("MT")
    add_rules("utils.bin2c")
    add_files("tools/webview2_host/**.cpp", "tools/browser_shared/BrowserPipe.cpp")
    add_files("tools/webview2_host/scripts/**.js", { rule = "utils.bin2c" })
    add_files("tools/browser_shared/performance-overlay.js", { rule = "utils.bin2c" })
    add_files("tools/webview2_host/audio-extension/manifest.json",
        "tools/webview2_host/audio-extension/collector.html",
        "tools/webview2_host/audio-extension/collector.js", { rule = "utils.bin2c" })
    add_files("src/Compat/V1/web-transport.js", { rule = "utils.bin2c" })
    add_headerfiles("tools/webview2_host/**.h", "tools/browser_shared/**.h")
    add_includedirs("src", "tools/browser_shared")
    add_packages("nlohmann_json")
    add_syslinks(
        "d3d11", "dxgi", "windowsapp", "runtimeobject", "CoreMessaging", "winmm",
        "ole32", "oleaut32", "uuid", "advapi32", "user32", "shell32", "taskschd", "comsuppw")
    add_ldflags("/SUBSYSTEM:WINDOWS", { force = true })
    on_load(function(target)
        local sdk = os.getenv("WEBVIEW2_SDK_DIR")
        if not sdk or sdk == "" then
            sdk = path.join(os.projectdir(), "external", "webview2")
        end
        local native = path.join(sdk, "build", "native")
        if not os.isfile(path.join(native, "include", "WebView2.h")) then
            raise("OSFUI WebView2 host: unpack Microsoft.Web.WebView2 into " ..
                "external/webview2 or set WEBVIEW2_SDK_DIR to the NuGet package root")
        end
        target:add("includedirs", path.join(native, "include"))
        target:add("linkdirs", path.join(native, "x64"))
        target:add("links", "WebView2LoaderStatic")
    end)

-- Windows-only transport tests stay separate from the portable native suite.
target("wv2-capture-timing-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/wv2_capture_timing_tests.cpp")
    add_includedirs("tools/webview2_host", "tests/native/stubs")
    add_syslinks("winmm")

target("wv2-capture-performance-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/wv2_capture_performance_tests.cpp")
    add_includedirs("tools/webview2_host", "tests/native/stubs")
    add_packages("nlohmann_json")

target("wv2-performance-gate-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/wv2_performance_gate_tests.cpp")
    add_includedirs("src", "tests/native/stubs")
    add_packages("nlohmann_json")

target("wv2-cdp-smoke")
    set_kind("binary")
    set_default(false)
    set_runtimes("MT")
    add_files("tests/native/wv2_cdp_smoke.cpp")
    add_includedirs("src", "tools/browser_shared")
    add_packages("nlohmann_json")
    add_syslinks("ole32", "user32", "version", "shlwapi", "advapi32")
    on_load(function(target)
        local sdk = os.getenv("WEBVIEW2_SDK_DIR") or path.join(os.projectdir(), "external", "webview2")
        target:add("includedirs", path.join(sdk, "build", "native", "include"))
        target:add("linkdirs", path.join(sdk, "build", "native", "x64"))
        target:add("links", "WebView2LoaderStatic")
    end)

target("wv2-pipe-tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/native/wv2_pipe_tests.cpp", "tools/browser_shared/BrowserPipe.cpp")
    add_includedirs("tools/browser_shared", "tests/native/stubs")
    add_syslinks("advapi32")

-- target name == repo folder == MO2 mod folder (deploy goes to XSE_SF_MODS_PATH\<target name>)
target("OSF UI")
    -- Keep the binary basename independent of the MO2 folder name.
    set_basename("OSFUI")
    add_defines("OSFUI_BROWSER_CEF=" .. (get_config("browser_backend") == "cef" and "1" or "0"))
    add_rules("commonlibsf.plugin", {
        name = "OSF UI",
        author = "ozooma10",
        description = "Web Interface Framework for Starfield (" .. (get_config("browser_backend") or "webview2") .. ")",
        email = "ozooma10@users.noreply.github.com"
    })

    add_packages("nlohmann_json")

    add_syslinks(
        "d3d12", "d3dcompiler", "shell32", "ole32", "xinput", "user32", "imm32",
        -- out-of-process host client (pipe ACL + Explorer/TaskScheduler broker)
        "oleaut32", "uuid", "comsuppw", "taskschd", "advapi32")

    add_files("src/**.cpp")
    add_headerfiles("src/**.h", "sdk/**.h")
    add_includedirs("src", "tools/browser_shared", "sdk")
    set_pcxxheader("src/pch.h")

    before_build(function(target)
        if os.getenv("XSE_SF_MODS_PATH") or os.getenv("XSE_SF_GAME_PATH") then
            import("runtime_payload", { rootdir = path.join(os.projectdir(), "tools", "xmake") })
            runtime_payload.build_papyrus()
        end
    end)
    -- Redeploy data even when the DLL itself did not change.
    after_build(function(target)
        import("runtime_payload", { rootdir = path.join(os.projectdir(), "tools", "xmake") })
        runtime_payload.deploy(target)
    end)

    -- `xmake install` skips before_build, so packaging needs this install hook.
    before_install(function(target)
        import("runtime_payload", { rootdir = path.join(os.projectdir(), "tools", "xmake") })
        runtime_payload.build_papyrus()
    end)
    after_install(function(target)
        import("runtime_payload", { rootdir = path.join(os.projectdir(), "tools", "xmake") })
        runtime_payload.install(target)
    end)

    -- The host must exist before data deployment packages it.
    add_deps("osfui-browser-broker")
    if get_config("browser_backend") == "cef" then
        add_deps("osfui-cef-host")
    else
        add_deps("osfui-webview2-host")
    end

-- Optional extension; the ordinary core build and package do not install it.
target("OSF UI - World Screens")
    set_default(false)
    set_basename("OSFUIWorldScreens")
    set_version("0.1.0")
    add_rules("commonlibsf.plugin", {name="OSF UI - World Screens", author="ozooma10", description="Optional material-backed world displays for OSF UI"})
    add_files("extensions/world-screens/src/**.cpp", "src/Composite/EngineD3D12.cpp", "src/Core/Json.cpp", "src/Core/Log.cpp")
    add_includedirs("extensions/world-screens/src", "src", "sdk", "tools/browser_shared")
    add_packages("nlohmann_json")
    add_syslinks("d3d12", "bcrypt", "ole32", "user32", "version")
    set_pcxxheader("src/pch.h")
    before_build(function(target)
        if os.getenv("XSE_SF_MODS_PATH") or os.getenv("XSE_SF_GAME_PATH") then
            import("world_screens_payload", {rootdir=path.join(os.projectdir(),"tools","xmake")})
            world_screens_payload.build_papyrus()
        end
    end)
    after_build(function(target)
        if os.getenv("XSE_SF_MODS_PATH") or os.getenv("XSE_SF_GAME_PATH") then
            import("world_screens_payload", {rootdir=path.join(os.projectdir(),"tools","xmake")})
            world_screens_payload.install(target)
        end
    end)
    before_install(function(target)
        import("world_screens_payload", {rootdir=path.join(os.projectdir(),"tools","xmake")})
        world_screens_payload.build_papyrus()
    end)
    after_install(function(target)
        import("world_screens_payload", {rootdir=path.join(os.projectdir(),"tools","xmake")})
        world_screens_payload.install(target)
    end)

target("osfui-screen-binding-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("extensions/world-screens/tests/screen_binding_tests.cpp")
    add_includedirs("extensions/world-screens/tests/stubs", "extensions/world-screens/src")

target("osfui-surface-catalog-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("extensions/world-screens/tests/surface_catalog_tests.cpp",
        "extensions/world-screens/src/World/SurfaceCatalog.cpp",
        "extensions/world-screens/src/World/WorldAssets.cpp")
    add_includedirs("extensions/world-screens/src", "src")
    add_syslinks("bcrypt")

target("osfui-game-consumer")
    set_default(false)
    set_basename("OSFUIBackendTest")
    add_rules("commonlibsf.plugin", {
        name = "OSF UI Backend Test", author = "OSF UI",
        description = "Disposable public API acceptance consumer"
    })
    add_files("tests/game/Consumer.cpp")
    add_includedirs("sdk")
    add_packages("nlohmann_json")

target("osfui-manifest-tests")
    add_undefines("NDEBUG")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/view_manifest_tests.cpp", "src/Views/ViewManifest.cpp", "src/Views/ViewManager.cpp", "src/Core/Json.cpp")
    add_includedirs("tests/native/stubs", "tests/native", "src", "sdk")
    set_pcxxheader("tests/native/stubs/pch.h")
    add_packages("nlohmann_json")

target("osfui-view-cache-tests")
    set_kind("binary")
    set_default(false)
    add_undefines("NDEBUG")
    add_tests("default")
    add_files("tests/native/view_cache_tests.cpp", "src/Views/ViewCache.cpp")
    add_includedirs("src")

target("osfui-compat-tests")
    set_kind("binary")
    set_default(false)
    set_rundir(os.projectdir())
    add_tests("default")
    add_files("tests/native/compat_v1_tests.cpp", "src/Compat/V1/SettingsCodec.cpp", "src/Compat/V1/KeyNames.cpp",
        "src/Compat/V1/LegacyViews.cpp", "src/Views/ViewManifest.cpp", "src/Views/ViewManager.cpp", "src/Core/Json.cpp")
    add_includedirs("tests/native/stubs", "tests/native", "src", "sdk")
    set_pcxxheader("tests/native/stubs/pch.h")
    add_packages("nlohmann_json")

target("osfui-legacy-bridge-tests")
    set_kind("binary")
    set_default(false)
    set_rundir(os.projectdir())
    add_tests("default")
    add_files("tests/native/legacy_bridge_tests.cpp", "src/Compat/V1/LegacyBridge.cpp",
        "src/Compat/V1/SettingsCodec.cpp", "src/Compat/V1/KeyNames.cpp", "src/API/BridgeApi.cpp",
        "src/Views/ViewRequestQueue.cpp", "src/Bridge/MessageBridge.cpp", "src/Core/Json.cpp")
    add_includedirs("tests/native/stubs", "tests/native", "src", "sdk")
    set_pcxxheader("tests/native/stubs/pch.h")
    add_packages("nlohmann_json")

target("osfui-raw-input-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/raw_input_packet_tests.cpp")
    add_includedirs("src", "tests/native/stubs")

target("osfui-shared-frame-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/shared_frame_state_tests.cpp")
    add_includedirs("src", "tests/native/stubs")

target("osfui-browser-recovery-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/browser_host_recovery_tests.cpp")
    add_includedirs("src", "tests/native/stubs")

target("osfui-browser-retirement-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/browser_host_retirement_tests.cpp")
    add_includedirs("src", "tools/browser_shared", "tests/native/stubs")

target("osfui-hardware-cursor-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/hardware_cursor_tests.cpp", "src/Input/HardwareCursor.cpp")
    add_includedirs("tests/native/cursor_stubs", "tests/native/stubs", "src")
    set_pcxxheader("tests/native/stubs/pch.h")

target("osfui-browser-backend-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/browser_backend_tests.cpp")
    add_includedirs("tests/native/stubs", "src", "tools/browser_shared")
    add_packages("nlohmann_json")
    set_pcxxheader("tests/native/stubs/pch.h")

target("osfui-remote-uri-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/remote_uri_tests.cpp")
    add_includedirs("tests/native/stubs")

target("osfui-browser-surface-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/browser_surface_tests.cpp")
    add_includedirs("tests/native/stubs", "tools/browser_shared")

target("osfui-webview2-navigation-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/wv2_media_navigation_tests.cpp")
    add_includedirs("tests/native/stubs", "src")
    add_packages("nlohmann_json")

target("osfui-cef-assets-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/cef_assets_tests.cpp")
    add_includedirs("tests/native/stubs")

target("osfui-cef-navigation-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/cef_navigation_tests.cpp")
    add_includedirs("tests/native/stubs")

target("osfui-cef-frame-pacer-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/cef_frame_pacer_tests.cpp")
    add_includedirs("tests/native/stubs")

target("osfui-cef-paint-damage-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/cef_paint_damage_tests.cpp")
    add_includedirs("tests/native/stubs")

for _, backend in ipairs({"webview2", "cef"}) do
    target(backend == "cef" and "osfui-runtime-lifecycle-cef-tests" or "osfui-runtime-lifecycle-tests")
        set_kind("binary")
        set_default(false)
        add_defines("OSFUI_BROWSER_CEF=" .. (backend == "cef" and "1" or "0"))
        for _, scenario in ipairs({"normal", "developer-mode", "stale-selection", "missing-settings", "renderer-init-failure",
            "input-hook-failure", "focus-menu-failure", "menu-events-failure", "suppression-failure",
            "recovery", "game-device-failure", "surfaces", "surface-input", "surface-fullscreen",
            "browser-surface", "browser-disabled", "browser-consent-on", "browser-consent-off", "browser-consent-failed", "surface-reload"}) do
            add_tests(scenario, {runargs = {scenario, backend}})
        end
        add_files("tests/native/runtime_lifecycle_tests.cpp", "tests/native/runtime_doubles.cpp",
            "src/Surfaces/SurfaceService.cpp", "tests/native/surface_frames_double.cpp",
            "src/Runtime/Runtime.cpp", "src/Runtime/RuntimeFrame.cpp", "src/Views/RuntimeViews.cpp",
            "src/Bridge/RuntimeBridge.cpp", "src/Input/RuntimeInput.cpp", "src/Input/InputCaptureController.cpp",
            "src/Input/PointerInputState.cpp", "src/Input/RelativePointerSession.cpp",
            "src/Input/GamepadSession.cpp", "src/Input/ViewInputGrants.cpp",
            "src/Views/ViewManifest.cpp", "src/Views/ViewManager.cpp", "src/Views/ViewPresentationController.cpp",
            "src/Views/ViewOpenCoordinator.cpp", "src/Views/ViewRevealGate.cpp", "src/Views/ViewRequestQueue.cpp",
            "src/Views/ViewLoadTracker.cpp", "src/Views/ViewRecoveryTracker.cpp",
            "src/API/BridgeApi.cpp", "src/Bridge/MessageBridge.cpp", "src/Bridge/RetainedStateStore.cpp",
            "src/Dependency/OSFSettingsClient.cpp", "src/Core/Json.cpp")
        add_includedirs("tests/native/stubs", "tests/native", "src", "sdk", "tools/browser_shared")
        set_pcxxheader("tests/native/stubs/pch.h")
        add_packages("nlohmann_json")
    target_end()
end

target("osfui-settings-client-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/settings_client_tests.cpp", "src/Dependency/OSFSettingsClient.cpp",
        "src/API/BridgeApi.cpp", "src/Views/ViewRequestQueue.cpp", "src/Bridge/MessageBridge.cpp", "src/Core/Json.cpp")
    add_includedirs("tests/native/stubs", "tests/native", "src", "sdk")
    set_pcxxheader("tests/native/stubs/pch.h")
    add_packages("nlohmann_json")

target("osfui-wv2-message-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/wv2_messages_tests.cpp", "src/Core/Json.cpp")
    add_includedirs("src", "tests/native/stubs", "tools/browser_shared")
    set_pcxxheader("tests/native/stubs/pch.h")
    add_packages("nlohmann_json")


target("osfui-view-reveal-tests")
    set_kind("binary")
    set_default(false)
    add_undefines("NDEBUG")
    add_tests("default")
    add_files("tests/native/view_reveal_gate_tests.cpp", "src/Views/ViewRevealGate.cpp")
    add_includedirs("src")

target("osfui-view-open-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/view_open_coordinator_tests.cpp", "src/Views/ViewOpenCoordinator.cpp", "src/Views/ViewPresentationController.cpp")
    add_includedirs("src", "tests/native/stubs")
    set_pcxxheader("tests/native/stubs/pch.h")
    add_packages("nlohmann_json")

target("osfui-view-request-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/view_request_queue_tests.cpp", "src/Views/ViewRequestQueue.cpp")
    add_includedirs("src", "tests/native/stubs")

target("osfui-bridge-api-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/bridge_api_tests.cpp", "src/API/BridgeApi.cpp", "src/Views/ViewRequestQueue.cpp", "src/Bridge/MessageBridge.cpp", "src/Core/Json.cpp")
    add_includedirs("src", "sdk", "tests/native/stubs")
    set_pcxxheader("tests/native/stubs/pch.h")
    add_packages("nlohmann_json")

target("osfui-api-version-tests")
    set_kind("binary")
    set_default(false)
    add_tests("default")
    add_files("tests/native/api_version_tests.cpp", "src/API/Exports.cpp", "src/API/BridgeApi.cpp", "src/Views/ViewRequestQueue.cpp", "src/Bridge/MessageBridge.cpp", "src/Core/Json.cpp")
    add_includedirs("src", "sdk", "tests/native/stubs")
    set_pcxxheader("tests/native/stubs/pch.h")
    add_packages("nlohmann_json")

-- Frozen Papyrus signatures through the real protocol bridge, with VM/Settings test doubles.
target("osfui-legacy-papyrus-tests")
    set_kind("binary")
    set_default(false)
    set_rundir(os.projectdir())
    add_tests("default")
    add_files("tests/native/legacy_papyrus_tests.cpp", "src/Compat/V1/PapyrusAdapter.cpp",
        "src/API/PapyrusApi.cpp", "src/API/BridgeApi.cpp", "src/Compat/V1/LegacyBridge.cpp",
        "src/Compat/V1/SettingsCodec.cpp", "src/Compat/V1/KeyNames.cpp",
        "src/Views/ViewRequestQueue.cpp", "src/Bridge/MessageBridge.cpp", "src/Core/Json.cpp")
    add_includedirs("tests/native/stubs", "tests/native", "src", "sdk")
    set_pcxxheader("tests/native/stubs/pch.h")
    add_packages("nlohmann_json")
