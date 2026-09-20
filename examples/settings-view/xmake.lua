local repo = path.absolute("../..", os.scriptdir())
local example = os.scriptdir()
includes(path.join(repo, "lib/commonlibsf"))

set_project("OSF UI Settings Example")
set_version("1.0.0")
set_languages("c++23")
set_arch("x64")
add_rules("mode.debug", "mode.releasedbg")

target("OSF UI Settings Example")
    set_basename("OSFUISettingsExample")
    add_rules("commonlibsf.plugin", {
        name = "OSF UI Settings Example",
        author = "OSF UI",
        description = "Development example: Slim settings and hotkey open an OSF UI view"
    })
    add_files("src/*.cpp")
    add_includedirs("src", path.join(repo, "sdk"), path.join(repo, "lib/osf-settings/sdk"))
    after_install(function(target)
        os.cp(path.join(example, "data", "*"), target:installdir())
    end)
