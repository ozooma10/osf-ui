-- include subprojects
includes("lib/commonlibsf")

-- set project constants
set_project("OSF UI")
set_version("1.3.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- Production WebView2 renderer backend: the plugin-side renderer plus the
-- out-of-process host. Uses only the static loader from the
-- Microsoft.Web.WebView2 NuGet package; the evergreen runtime remains an OS
-- dependency. The package may be unpacked to external/webview2, or
-- WEBVIEW2_SDK_DIR may point at its package root.
option("with_webview2", function()
    set_default(true)
    set_showmenu(true)
    set_description("Build the WebView2 renderer and out-of-process host")
end)

-- JSON for config, view manifests, and the message bridge
add_requires("nlohmann_json")

-- The host exe is required by the plugin's "webview2" renderer.
if has_config("with_webview2") then
    -- Production host executable. Self-contained on purpose: static CRT +
    -- static WebView2 loader, because it runs from a mirrored real path
    -- (%LOCALAPPDATA%\OSFUI\bin\<version>) with no neighbours.
    target("osfui-webview2-host")
        set_kind("binary")
        set_basename("osfui_webview2_host")
        set_default(false)
        set_languages("c++23")
        set_warnings("allextra")
        -- Interpret source (and encode narrow/wide literals) as UTF-8. Without
        -- /utf-8 MSVC decodes UTF-8 bytes as the system code page, mangling the
        -- non-ASCII characters in the render-stats overlay JS (LR"JS(...)JS" in
        -- HostApp.cpp) into mojibake on screen.
        set_encodings("utf-8")
        set_runtimes("MT")
        add_rules("mode.debug", "mode.releasedbg")
        add_files("tools/webview2_host/**.cpp", "tools/webview2_shared/**.cpp")
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

end
-- define targets
-- target name == repo folder == MO2 mod folder (deploy goes to XSE_SF_MODS_PATH\<target name>)
target("OSF UI")
    -- DLL basename (target name has a space for the MO2 folder; the binary itself is the space-free "OSFUI.dll"). 
    -- Data folder is SFSE/Plugins/OSFUI/
    set_basename("OSFUI")
    -- Compile UTF-8 source with UTF-8 narrow/wide literals (see the host target
    -- note above); the commonlibsf subproject sets this for itself but it does
    -- not propagate to this target.
    set_encodings("utf-8")
    add_rules("commonlibsf.plugin", {
        name = "OSF UI",
        author = "ozooma10",
        description = "Web Interface Framework for Starfield",
        email = "ozooma10@users.noreply.github.com"
    })

    -- add packages
    add_packages("nlohmann_json")

    -- D3D12 overlay compositor (composite/): the device/queue are the game's, but we still need these for our own root signature, pipeline state, shader compile, and the swapchain present hook. 
    -- d3dcompiler is used to build the overlay shaders at runtime
    -- shell32/ole32: SHGetKnownFolderPath for the writable settings path.
    add_syslinks("d3d12", "dxgi", "d3dcompiler", "shell32", "ole32")

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    -- sdk/ holds the public single-header native API (OSFUI_API.h); 
    -- src/api includes it directly so the impl and the consumer copy share one ABI def.
    add_headerfiles("sdk/OSFUI_API.h")
    add_includedirs("sdk")
    set_pcxxheader("src/pch.h")

    -- ship the plugin data folder (config + views) next to the DLL:
    -- <install>/SFSE/Plugins/OSFUI/...
    add_installfiles("data/(OSFUI/**)", { prefixdir = "SFSE/Plugins" })
    -- the Papyrus surface (authoring-settings.md "From Papyrus"): loose scripts
    -- at the Data root -- <install>/Scripts/OSFUI.pex (+ Source/OSFUI.psc)
    add_installfiles("data/(Scripts/**)")

    -- Redeploy data/ (views + config) to the mod folder on every build where a data file changed. 
    -- The commonlib rule's after_build only runs "xmake install" when the DLL binary itself changed, so pure HTML/JS/JSON edits would otherwise never reach XSE_SF_MODS_PATH.
    after_build(function(target)
        if not (os.getenv("XSE_SF_MODS_PATH") or os.getenv("XSE_SF_GAME_PATH")) then
            return
        end
        import("core.project.depend")
        local datadir = path.join(os.projectdir(), "data", "OSFUI")
        local scriptsdir = path.join(os.projectdir(), "data", "Scripts")
        local files = os.files(path.join(os.projectdir(), "data", "**"))
        depend.on_changed(function()
            local dstdir = path.join(target:installdir(), "SFSE", "Plugins")
            os.cp(datadir, dstdir)
            -- Papyrus surface: loose scripts at the Data root (mod folder root)
            os.cp(scriptsdir, target:installdir())
            cprint("${dim}deploying data/OSFUI + data/Scripts to %s ..", target:installdir())
        end, { files = files, values = files,
               dependfile = target:dependfile("osfui_data_deploy") })
        -- Out-of-process WebView2 host: ship the exe inside the plugin data
        -- dir (SFSE/Plugins/OSFUI/bin). The renderer mirrors it to a REAL
        -- path outside the MO2 VFS at runtime before broker-launching it.
        import("core.project.config")
        if config.get("with_webview2") then
            import("core.project.project")
            local host = project.target("osfui-webview2-host")
            if host and os.isfile(host:targetfile()) then
                local bindir = path.join(target:installdir(), "SFSE", "Plugins", "OSFUI", "bin")
                os.mkdir(bindir)
                os.cp(host:targetfile(), path.join(bindir, "osfui_webview2_host.exe"))
                cprint("${dim}deploying osfui_webview2_host.exe to %s ..", bindir)
            end
        end
    end)

    -- xmake install is also used by the release packager. Install the
    -- production host explicitly; unlike the MO2 auto-deploy above, this path
    -- runs with XSE_SF_* unset and must not depend on an after-build side effect.
    after_install(function(target)
        import("core.project.config")
        if config.get("with_webview2") then
            import("core.project.project")
            local host = project.target("osfui-webview2-host")
            if not host or not os.isfile(host:targetfile()) then
                raise("OSFUI WebView2 host was not built; cannot install a runnable plugin")
            end
            local bindir = path.join(target:installdir(), "SFSE", "Plugins", "OSFUI", "bin")
            os.mkdir(bindir)
            os.cp(host:targetfile(), path.join(bindir, "osfui_webview2_host.exe"))
        end
    end)

    if has_config("with_webview2") then
        add_defines("OSFUI_WITH_WEBVIEW2=1")
        add_syslinks(
            "d3d11", "dcomp", "windowsapp", "runtimeobject", "CoreMessaging",
            "shlwapi", "user32", "gdi32", "version",
            -- out-of-process host client (pipe ACL + Explorer/TaskScheduler broker)
            "oleaut32", "uuid", "comsuppw", "taskschd", "advapi32")
        -- Wv2SharedCompat.cpp compiles tools/webview2_shared through the pch.
        add_includedirs("tools/webview2_shared")
        -- Host exe must exist before the data deploy below can package it.
        add_deps("osfui-webview2-host")
    else
        remove_files("src/render/WebView2HostWebRenderer.cpp")
    end
    -- NOTE: the plugin deliberately does NOT link the WebView2 SDK. Everything
    -- that touches the browser lives in osfui_webview2_host.exe; this DLL only
    -- speaks the pipe protocol. Keeping the proprietary loader out of the
    -- GPL-3.0 plugin is intentional — do not add it back for a convenience
    -- call. If you need runtime facts (version, availability), read them from
    -- the host's hello handshake.
