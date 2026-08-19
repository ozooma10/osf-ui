includes("lib/commonlibsf")

set_project("OSF UI")
set_version("2.0.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- JSON for settings, view manifests, and the message bridge
add_requires("nlohmann_json")

-- The mirrored host is self-contained: static CRT and WebView2 loader.
target("osfui-webview2-host")
        set_kind("binary")
        set_basename("osfui_webview2_host")
        set_default(false)
        set_languages("c++23")
        set_warnings("allextra")
        -- Keep non-ASCII log and UI literals independent of the system code page.
        set_encodings("utf-8")
        set_runtimes("MT")
        add_rules("mode.debug", "mode.releasedbg", "utils.bin2c")
        add_files("tools/webview2_host/**.cpp", "tools/webview2_shared/**.cpp")
        add_files("tools/webview2_host/scripts/**.js", { rule = "utils.bin2c" })
        add_headerfiles("tools/webview2_host/**.h", "tools/webview2_shared/**.h")
        add_includedirs("src", "tools/webview2_shared")
        add_packages("nlohmann_json")
        add_syslinks(
            "d3d11", "dxgi", "windowsapp", "runtimeobject", "CoreMessaging",
            "ole32", "oleaut32", "uuid", "comsuppw", "taskschd", "advapi32",
            "user32", "shell32")
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
    set_languages("c++23")
    set_warnings("allextra")
    set_encodings("utf-8")
    add_files("tests/native/wv2_pipe_tests.cpp", "tools/webview2_shared/Wv2Pipe.cpp")
    add_includedirs("tools/webview2_shared", "tests/native/stubs")
    add_syslinks("advapi32")

-- target name == repo folder == MO2 mod folder (deploy goes to XSE_SF_MODS_PATH\<target name>)
target("OSF UI")
    -- Keep the binary basename independent of the MO2 folder name.
    set_basename("OSFUI")
    -- CommonLibSF's UTF-8 setting does not propagate to this target.
    set_encodings("utf-8")
    add_rules("commonlibsf.plugin", {
        name = "OSF UI",
        author = "ozooma10",
        description = "Web Interface Framework for Starfield",
        email = "ozooma10@users.noreply.github.com"
    })

    add_packages("nlohmann_json")

    add_syslinks("d3d12", "dxgi", "dxguid", "d3dcompiler", "shell32", "ole32", "xinput")

    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    add_includedirs("tools/webview2_shared")
    -- The implementation and consumers compile against the same public ABI header.
    add_headerfiles("sdk/OSFUI_API.h", "sdk/OSFUI_JSON.h")
    add_includedirs("sdk")
    set_pcxxheader("src/pch.h")

    -- Authored data and generated views are installed through separate paths.
    add_installfiles("data/(OSFUI/**)", { prefixdir = "SFSE/Plugins" })
    -- Papyrus scripts remain loose at the Data root.
    add_installfiles("data/(Scripts/**)")
    -- Build the ignored frontend artifact before deployment.
    before_build(function(target)
        import("frontend_views", { rootdir = path.join(os.projectdir(), "tools", "xmake") })
        frontend_views.build()
    end)
    -- Redeploy data even when the DLL itself did not change.
    after_build(function(target)
        if not (os.getenv("XSE_SF_MODS_PATH") or os.getenv("XSE_SF_GAME_PATH")) then
            return
        end
        import("core.project.depend")
        import("core.project.config")
        local datadir = path.join(os.projectdir(), "data", "OSFUI")
        local scriptsdir = path.join(os.projectdir(), "data", "Scripts")
        local viewsdir = path.join(os.projectdir(), "build", "frontend", "views")
        local files = os.files(path.join(os.projectdir(), "data", "**"))
        table.join2(files, os.files(path.join(viewsdir, "**")))
        depend.on_changed(function()
            local dstdir = path.join(target:installdir(), "SFSE", "Plugins")
            os.cp(datadir, dstdir)
            local deployed = path.join(dstdir, "OSFUI", "views")
            os.rm(path.join(deployed, "shared"))
            os.rm(path.join(deployed, "osfui"))
            os.cp(viewsdir, path.join(dstdir, "OSFUI"))
            -- Papyrus API: loose scripts at the Data root (mod folder root)
            os.cp(scriptsdir, target:installdir())
            cprint("${dim}deploying data/OSFUI + generated views + data/Scripts to %s ..", target:installdir())
        end, { files = files, values = files,
               dependfile = target:dependfile("osfui_data_deploy") })
        -- Ship the host inside plugin data; runtime mirrors it outside the MO2 VFS.
        import("core.project.project")
        local host = project.target("osfui-webview2-host")
        if host and os.isfile(host:targetfile()) then
            local bindir = path.join(target:installdir(), "SFSE", "Plugins", "OSFUI", "bin")
            os.mkdir(bindir)
            os.cp(host:targetfile(), path.join(bindir, "osfui_webview2_host.exe"))
            cprint("${dim}deploying osfui_webview2_host.exe to %s ..", bindir)
        end
    end)

    -- `xmake install` skips before_build, so packaging needs this install hook.
    before_install(function(target)
        import("frontend_views", { rootdir = path.join(os.projectdir(), "tools", "xmake") })
        frontend_views.build()
    end)
    after_install(function(target)
        import("core.project.config")
        local viewsdir = path.join(os.projectdir(), "build", "frontend", "views")
        local datadir = path.join(target:installdir(), "SFSE", "Plugins", "OSFUI")
        os.rm(path.join(datadir, "views"))
        os.cp(viewsdir, datadir)
        import("core.project.project")
        local host = project.target("osfui-webview2-host")
        if not host or not os.isfile(host:targetfile()) then
            raise("OSFUI WebView2 host was not built; cannot install a runnable plugin")
        end
        local bindir = path.join(target:installdir(), "SFSE", "Plugins", "OSFUI", "bin")
        os.mkdir(bindir)
        os.cp(host:targetfile(), path.join(bindir, "osfui_webview2_host.exe"))
    end)

    add_syslinks(
        "d3d11", "dcomp", "windowsapp", "runtimeobject", "CoreMessaging",
        "shlwapi", "user32", "gdi32", "version",
        -- out-of-process host client (pipe ACL + Explorer/TaskScheduler broker)
        "oleaut32", "uuid", "comsuppw", "taskschd", "advapi32")
    -- The host must exist before data deployment packages it.
    add_deps("osfui-webview2-host")
    -- Keep the proprietary WebView2 loader in the host; the GPL plugin only speaks the pipe protocol.
