-- include subprojects
includes("lib/commonlibsf")

-- set project constants
set_project("OSF UI")
set_version("1.0.0")
set_license("GPL-3.0")
set_languages("c++23")
set_warnings("allextra")

-- add common rules
add_rules("mode.debug", "mode.releasedbg")
add_rules("plugin.vsxmake.autoupdate")

-- optional Ultralight renderer backend.
-- The Ultralight SDK is proprietary and is NEVER vendored into this repository.
-- Enable with:  xmake f --with_ultralight=true   (requires ULTRALIGHT_SDK_DIR)
option("with_ultralight", function()
    set_default(false)
    set_showmenu(true)
    set_description("Enable the Ultralight web renderer backend (requires ULTRALIGHT_SDK_DIR env var)")
end)
-- Standalone WebView2 composition/capture spike. This deliberately remains an
-- opt-in executable and does not alter the plugin or its Ultralight backend.
-- Enable with: xmake f --with_webview2_poc=true
-- The Microsoft.Web.WebView2 NuGet package may be unpacked to
-- external/webview2, or WEBVIEW2_SDK_DIR may point at its package root.
option("with_webview2_poc", function()
    set_default(false)
    set_showmenu(true)
    set_description("Build the standalone WebView2 composition/capture proof of concept")
end)
-- In-process WebView2 renderer backend. Opt-in until the Phase 2 in-game gates pass.
-- Uses only the static loader from the Microsoft.Web.WebView2 NuGet package;
-- the evergreen runtime remains an OS dependency.
option("with_webview2", function()
    set_default(false)
    set_showmenu(true)
    set_description("Enable the experimental in-process WebView2 renderer backend")
end)

-- JSON for config, view manifests, and the message bridge
add_requires("nlohmann_json")

if has_config("with_webview2_poc") then
    target("osfui-webview2-poc")
        set_kind("binary")
        set_default(false)
        set_languages("c++23")
        set_warnings("allextra")
        add_rules("mode.debug", "mode.releasedbg")
        add_files("tools/webview2_poc/**.cpp")
        add_headerfiles("tools/webview2_poc/**.h")
        add_syslinks(
            "d3d11", "dxgi", "d3dcompiler", "dcomp", "windowsapp",
            "runtimeobject", "CoreMessaging", "shlwapi", "shell32", "ole32", "user32",
            "gdi32", "version")
        add_ldflags("/SUBSYSTEM:WINDOWS", { force = true })

        on_load(function(target)
            local sdk = os.getenv("WEBVIEW2_SDK_DIR")
            if not sdk or sdk == "" then
                sdk = path.join(os.projectdir(), "external", "webview2")
            end
            local native = path.join(sdk, "build", "native")
            local header = path.join(native, "include", "WebView2.h")
            local loader = path.join(native, "x64", "WebView2LoaderStatic.lib")
            if not os.isfile(header) or not os.isfile(loader) then
                raise("OSFUI WebView2 POC: unpack Microsoft.Web.WebView2 into " ..
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
    end)

    if has_config("with_webview2") then
        add_defines("OSFUI_WITH_WEBVIEW2=1")
        add_syslinks(
            "d3d11", "dcomp", "windowsapp", "runtimeobject", "CoreMessaging",
            "shlwapi", "user32", "gdi32", "version")
        on_load(function(target)
            local sdk = os.getenv("WEBVIEW2_SDK_DIR")
            if not sdk or sdk == "" then
                sdk = path.join(os.projectdir(), "external", "webview2")
            end
            local native = path.join(sdk, "build", "native")
            local header = path.join(native, "include", "WebView2.h")
            local loader = path.join(native, "x64", "WebView2LoaderStatic.lib")
            if not os.isfile(header) or not os.isfile(loader) then
                raise("OSFUI WebView2: unpack Microsoft.Web.WebView2 into " ..
                    "external/webview2 or set WEBVIEW2_SDK_DIR to the NuGet package root")
            end
            target:add("includedirs", path.join(native, "include"))
            target:add("linkdirs", path.join(native, "x64"))
            target:add("links", "WebView2LoaderStatic")
        end)
    else
        remove_files("src/render/WebView2WebRenderer.cpp")
    end
    if has_config("with_ultralight") then
        add_defines("OSFUI_WITH_ULTRALIGHT=1")
        on_load(function(target)
            local sdk = os.getenv("ULTRALIGHT_SDK_DIR")
            if not sdk or sdk == "" then
                raise("OSFUI: with_ultralight=true requires the ULTRALIGHT_SDK_DIR environment variable to point at a local Ultralight SDK (https://ultralig.ht). ")
            end
            if not os.isdir(path.join(sdk, "include")) then
                raise("OSFUI: ULTRALIGHT_SDK_DIR is set but '" .. path.join(sdk, "include") .. "' does not exist. Point ULTRALIGHT_SDK_DIR at the SDK root.")
            end
            target:add("includedirs", path.join(sdk, "include"))
            target:add("linkdirs", path.join(sdk, "lib"))
            -- AppCore is linked ONLY for GetPlatformFontLoader (DirectWrite); no window/app machinery is used (offscreen rendering only).
            -- Symbol homes: the C++ core API (Platform/String/Buffer/BitmapSurface) is in UltralightCore, JavaScriptCore's C API is in WebCore.
            target:add("links", "Ultralight", "UltralightCore", "WebCore", "AppCore")
            -- Delay-load the SDK DLLs: SFSE loads plugins with plain LoadLibrary, so static imports would never resolve from the plugin's folder. 
            -- UltralightWebRenderer::Initialize preloads the DLLs from <data>/ultralight/bin before first use.
            target:add("syslinks", "delayimp")
            -- this target is a shared lib, so xmake feeds the linker from "shflags" (plain ldflags are silently ignored here).
            target:add("shflags",
                "/DELAYLOAD:Ultralight.dll",
                "/DELAYLOAD:UltralightCore.dll",
                "/DELAYLOAD:WebCore.dll",
                "/DELAYLOAD:AppCore.dll",
                { force = true })
            -- Ship the runtime pieces with the plugin data folder:
            --   SFSE/Plugins/OSFUI/ultralight/bin/*.dll
            --   SFSE/Plugins/OSFUI/ultralight/resources/icudt67l.dat
            -- cacert.pem is intentionally NOT shipped: network stays off (docs/security-model.md), so no TLS roots are needed.
            target:add("installfiles", path.join(sdk, "bin", "(*.dll)"),
                { prefixdir = "SFSE/Plugins/OSFUI/ultralight/bin" })
            target:add("installfiles", path.join(sdk, "resources", "(icudt67l.dat)"),
                { prefixdir = "SFSE/Plugins/OSFUI/ultralight/resources" })
            -- Ship Ultralight's license texts next to its binaries so the required attribution travels with the distributed mod.
            target:add("installfiles", path.join(sdk, "license", "(**)"),
                { prefixdir = "SFSE/Plugins/OSFUI/ultralight/license" })
        end)
    else
        -- UltralightWebRenderer.cpp is also fully #if-guarded, but exclude it outright so the default build never touches it.
        remove_files("src/render/UltralightWebRenderer.cpp")
    end
