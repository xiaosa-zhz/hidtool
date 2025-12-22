add_rules("mode.debug", "mode.release")
set_languages("c++23")
add_includedirs("inc")
add_files("src/*.cpp")
add_rules("plugin.compile_commands.autoupdate", {outputdir = "./"})

target("hidtool")
    set_kind("binary")
