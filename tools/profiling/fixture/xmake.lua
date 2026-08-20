set_xmakever("3.0.0")

set_project("UIBench")
set_version("1.0.0")
set_arch("x64")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")

add_rules("mode.debug", "mode.releasedbg")
add_requires("nlohmann_json")
includes("../../../lib/commonlibsf")

target("UIBench")
    set_default(false)
    add_rules("commonlibsf.plugin", {
        name = "UIBench",
        author = "OSF UI performance toolbench",
        description = "Loads the identical UIBench document through OSF UI or Carbon UI",
        options = {
            address_library = false,
            sig_scanning = false,
            no_struct_use = true,
            layout_dependent = false
        }
    })
    add_files("src/**.cpp")
    add_headerfiles("include/**.h")
    add_includedirs("include", "../../../sdk")
    add_packages("nlohmann_json")
    set_installdir("build/install")
