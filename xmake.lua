includes("lib/commonlibsf")

set_project("OSF UI")
set_version("2.0.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- JSON for settings, view manifests, and the message bridge
add_requires("nlohmann_json")

local function build_frontend_views()
    import("frontend_views", { rootdir = path.join(os.projectdir(), "tools", "xmake") })
    frontend_views.build()
end

-- The mirrored host is self-contained: static CRT and WebView2 loader.
target("osfui-webview2-host")
    set_kind("binary")
    set_basename("osfui_webview2_host")
    set_default(false)
    set_runtimes("MT")
    add_rules("utils.bin2c")
    add_files("tools/webview2_host/**.cpp", "tools/webview2_shared/Wv2Pipe.cpp")
    add_files("tools/webview2_host/scripts/**.js", { rule = "utils.bin2c" })
    add_headerfiles("tools/webview2_host/**.h", "tools/webview2_shared/**.h")
    add_includedirs("src", "tools/webview2_shared")
    add_packages("nlohmann_json")
    add_syslinks(
        "d3d11", "dxgi", "windowsapp", "runtimeobject", "CoreMessaging",
        "ole32", "oleaut32", "uuid", "advapi32", "user32", "shell32")
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
target("wv2-pipe-tests")
    set_kind("binary")
    set_default(false)
    add_files("tests/native/wv2_pipe_tests.cpp", "tools/webview2_shared/Wv2Pipe.cpp")
    add_includedirs("tools/webview2_shared", "tests/native/stubs")
    add_syslinks("advapi32")

-- target name == repo folder == MO2 mod folder (deploy goes to XSE_SF_MODS_PATH\<target name>)
target("OSF UI")
    -- Keep the binary basename independent of the MO2 folder name.
    set_basename("OSFUI")
    add_rules("commonlibsf.plugin", {
        name = "OSF UI",
        author = "ozooma10",
        description = "Web Interface Framework for Starfield",
        email = "ozooma10@users.noreply.github.com"
    })

    add_packages("nlohmann_json")

    add_syslinks(
        "d3d12", "d3dcompiler", "shell32", "ole32", "xinput", "user32",
        -- out-of-process host client (pipe ACL + Explorer/TaskScheduler broker)
        "oleaut32", "uuid", "comsuppw", "taskschd", "advapi32")

    add_files("src/**.cpp")
    -- The implementation and consumers compile against the same public ABI header.
    add_headerfiles("src/**.h", "sdk/OSFUI_API.h", "sdk/OSFUI_JSON.h")
    add_includedirs("src", "tools/webview2_shared", "sdk")
    set_pcxxheader("src/pch.h")

    before_build(build_frontend_views)
    -- Redeploy data even when the DLL itself did not change.
    after_build(function(target)
        import("runtime_payload", { rootdir = path.join(os.projectdir(), "tools", "xmake") })
        runtime_payload.deploy(target)
    end)

    -- `xmake install` skips before_build, so packaging needs this install hook.
    before_install(build_frontend_views)
    after_install(function(target)
        import("runtime_payload", { rootdir = path.join(os.projectdir(), "tools", "xmake") })
        runtime_payload.install(target)
    end)

    -- The host must exist before data deployment packages it.
    add_deps("osfui-webview2-host")
