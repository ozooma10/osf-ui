-- include subprojects
includes("lib/commonlibsf")

-- set project constants
set_project("StarfieldWebUI")
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
target("StarfieldWebUI")
    add_rules("commonlibsf.plugin", {
        name = "StarfieldWebUI",
        author = "TODO",
        description = "HTML/CSS/JS UI runtime prototype for Starfield via SFSE/CommonLibSF",
        email = "user@site.com"
    })

    -- add packages
    add_packages("nlohmann_json")

    -- add src files
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    set_pcxxheader("src/pch.h")

    -- ship the plugin data folder (config + views) next to the DLL:
    -- <install>/SFSE/Plugins/StarfieldWebUI/...
    add_installfiles("data/(StarfieldWebUI/**)", { prefixdir = "SFSE/Plugins" })

    if has_config("with_ultralight") then
        add_defines("SWUI_WITH_ULTRALIGHT=1")
        on_load(function(target)
            local sdk = os.getenv("ULTRALIGHT_SDK_DIR")
            if not sdk or sdk == "" then
                raise("StarfieldWebUI: with_ultralight=true requires the ULTRALIGHT_SDK_DIR environment " ..
                      "variable to point at a local Ultralight SDK (https://ultralig.ht). " ..
                      "The SDK is proprietary and is not vendored in this repository.")
            end
            if not os.isdir(path.join(sdk, "include")) then
                raise("StarfieldWebUI: ULTRALIGHT_SDK_DIR is set but '" .. path.join(sdk, "include") ..
                      "' does not exist. Point ULTRALIGHT_SDK_DIR at the SDK root.")
            end
            target:add("includedirs", path.join(sdk, "include"))
            target:add("linkdirs", path.join(sdk, "lib"))
            -- AppCore is intentionally not linked: this project renders offscreen only.
            target:add("links", "Ultralight", "UltralightCore", "WebCore")
        end)
    else
        -- UltralightWebRenderer.cpp is also fully #if-guarded, but exclude it
        -- outright so the default build never touches it.
        remove_files("src/render/UltralightWebRenderer.cpp")
    end
