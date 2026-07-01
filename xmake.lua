-- include subprojects
includes("lib/commonlibsf")

-- set project constants
set_project("OSF UI")
set_version("0.1.0")
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

-- JSON for config, view manifests, and the message bridge
add_requires("nlohmann_json")

-- define targets
-- target name == repo folder == MO2 mod folder (deploy goes to XSE_SF_MODS_PATH\<target name>)
target("OSF UI")
    -- DLL basename (target name has a space for the MO2 folder; the binary
    -- itself is the space-free "OSFUI.dll"). Data folder is SFSE/Plugins/OSFUI/
    -- (src/core/Version.h kDataFolderName).
    set_basename("OSFUI")
    add_rules("commonlibsf.plugin", {
        -- plugin display name (SFSEPlugin_Version + SFSE log filename).
        name = "OSF UI",
        author = "ozooma10",
        description = "HTML/CSS/JS UI runtime prototype for Starfield via SFSE/CommonLibSF",
        email = "ozooma10@users.noreply.github.com"
    })

    -- add packages
    add_packages("nlohmann_json")

    -- D3D12 overlay compositor (composite/): the device/queue are the game's
    -- (located at runtime, not created), but we still need these for our own
    -- root signature, pipeline state, shader compile, and the swapchain
    -- present hook. d3dcompiler is used to build the overlay shaders at
    -- runtime (the game already loads D3DCompiler_47.dll).
    -- shell32/ole32: SHGetKnownFolderPath for the writable settings path.
    add_syslinks("d3d12", "dxgi", "d3dcompiler", "shell32", "ole32")

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    -- sdk/ holds the public single-header native API (OSFUI_API.h); src/api
    -- includes it directly so the impl and the consumer copy share one ABI def.
    add_headerfiles("sdk/OSFUI_API.h")
    add_includedirs("sdk")
    set_pcxxheader("src/pch.h")

    -- ship the plugin data folder (config + views) next to the DLL:
    -- <install>/SFSE/Plugins/OSFUI/...
    add_installfiles("data/(OSFUI/**)", { prefixdir = "SFSE/Plugins" })

    if has_config("with_ultralight") then
        add_defines("OSFUI_WITH_ULTRALIGHT=1")
        on_load(function(target)
            local sdk = os.getenv("ULTRALIGHT_SDK_DIR")
            if not sdk or sdk == "" then
                raise("OSFUI: with_ultralight=true requires the ULTRALIGHT_SDK_DIR environment " ..
                      "variable to point at a local Ultralight SDK (https://ultralig.ht). " ..
                      "The SDK is proprietary and is not vendored in this repository.")
            end
            if not os.isdir(path.join(sdk, "include")) then
                raise("OSFUI: ULTRALIGHT_SDK_DIR is set but '" .. path.join(sdk, "include") ..
                      "' does not exist. Point ULTRALIGHT_SDK_DIR at the SDK root.")
            end
            target:add("includedirs", path.join(sdk, "include"))
            target:add("linkdirs", path.join(sdk, "lib"))
            -- AppCore is linked ONLY for GetPlatformFontLoader (DirectWrite);
            -- no window/app machinery is used (offscreen rendering only).
            -- Symbol homes (verified against the 1.4.0 SDK libs): the C++
            -- core API (Platform/String/Buffer/BitmapSurface) is in
            -- UltralightCore, JavaScriptCore's C API is in WebCore.
            target:add("links", "Ultralight", "UltralightCore", "WebCore", "AppCore")
            -- Delay-load the SDK DLLs: SFSE loads plugins with plain
            -- LoadLibrary, so static imports would never resolve from the
            -- plugin's folder. UltralightWebRenderer::Initialize preloads the
            -- DLLs from <data>/ultralight/bin before first use.
            target:add("syslinks", "delayimp")
            -- NB: this target is a shared lib, so xmake feeds the linker from
            -- "shflags" (plain ldflags are silently ignored here).
            target:add("shflags",
                "/DELAYLOAD:Ultralight.dll",
                "/DELAYLOAD:UltralightCore.dll",
                "/DELAYLOAD:WebCore.dll",
                "/DELAYLOAD:AppCore.dll",
                { force = true })
            -- Ship the runtime pieces with the plugin data folder:
            --   SFSE/Plugins/OSFUI/ultralight/bin/*.dll
            --   SFSE/Plugins/OSFUI/ultralight/resources/icudt67l.dat
            -- cacert.pem is intentionally NOT shipped: network stays off
            -- (docs/security-model.md), so no TLS roots are needed.
            target:add("installfiles", path.join(sdk, "bin", "(*.dll)"),
                { prefixdir = "SFSE/Plugins/OSFUI/ultralight/bin" })
            target:add("installfiles", path.join(sdk, "resources", "(icudt67l.dat)"),
                { prefixdir = "SFSE/Plugins/OSFUI/ultralight/resources" })
            -- Ship Ultralight's license texts next to its binaries so the
            -- required attribution travels with the distributed mod. The Free
            -- License Agreement requires the NOTICES legend in the Licensed
            -- Product's credits (sec. 4.4, Marking) and that End Users receive
            -- the EULA (sec. 4.3). The SDK's license/ folder holds NOTICES.md,
            -- EULA.txt, and LICENSE.txt; ship all of it so no required notice
            -- is omitted. Only bundled when Ultralight itself is (this block).
            target:add("installfiles", path.join(sdk, "license", "(**)"),
                { prefixdir = "SFSE/Plugins/OSFUI/ultralight/license" })
        end)
    else
        -- UltralightWebRenderer.cpp is also fully #if-guarded, but exclude it
        -- outright so the default build never touches it.
        remove_files("src/render/UltralightWebRenderer.cpp")
    end
