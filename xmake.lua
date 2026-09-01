set_xmakever("3.0.9")
set_policy("package.requires_lock", true)

if is_plat("windows") then
    local project_root = os.projectdir()
    add_cxflags(
        "/Brepro",
        "/experimental:deterministic",
        '/d1trimfile:"' .. project_root .. '"',
        '/pathmap:"' .. project_root .. '"=.',
        {
            tools = "cl",
            force = true
        })
    add_shflags("/Brepro", "/PDBALTPATH:%_PDB%", {
        force = true
    })
end

includes(path.join(os.projectdir(), "lib", "commonlibsf"))

add_requires("nlohmann_json 3.11.3")
add_requires("freetype 2.14.1", {
    configs = {
        bzip2 = false,
        harfbuzz = false,
        png = false,
        shared = false,
        woff2 = false,
        zlib = false
    }
})

local plugin_name = "SFSE Menu Framework"
local dll_name = "SFSEMenuFramework"
local plugin_version = "0.9.0"
local plugin_author = "Quantumyilmaz"

set_project(plugin_name)
set_version(plugin_version)
set_license("GPL-3.0-only")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")

add_rules("mode.debug", "mode.releasedbg", "mode.release")
add_rules("plugin.vsxmake.autoupdate")

target("imgui", function()
    set_kind("static")
    set_default(false)
    set_license("MIT")

    add_files(
        "extern/imgui/imgui.cpp",
        "extern/imgui/imgui_draw.cpp",
        "extern/imgui/imgui_tables.cpp",
        "extern/imgui/imgui_widgets.cpp",
        "extern/imgui/backends/imgui_impl_dx12.cpp",
        "extern/imgui/backends/imgui_impl_win32.cpp",
        "extern/imgui_bridge/FontVariation.cpp",
        "extern/imgui_bridge/imgui_freetype_bridge.cpp"
    )
    add_headerfiles(
        "extern/imgui/imconfig.h",
        "extern/imgui/imgui.h",
        "extern/imgui/imgui_internal.h",
        "extern/imgui/imstb_rectpack.h",
        "extern/imgui/imstb_textedit.h",
        "extern/imgui/imstb_truetype.h",
        "extern/imgui/backends/imgui_impl_dx12.h",
        "extern/imgui/backends/imgui_impl_win32.h",
        "extern/imgui/misc/freetype/imgui_freetype.h",
        "extern/imgui_bridge/FontVariation.h"
    )
    add_defines("IMGUI_ENABLE_FREETYPE")
    add_includedirs("extern/imgui", { public = true })
    add_includedirs("extern/imgui_bridge", { public = true })
    add_packages("freetype", { public = true })
    add_syslinks("d3dcompiler", { public = true })
end)

target(dll_name, function()
	add_rules("commonlibsf.plugin", {
		author = plugin_author,
		name = plugin_name,
		description = plugin_name,
        options = {
            sig_scanning = false,
            address_library = true,
            no_struct_use = false,
            layout_dependent = true
        }
    })

    set_version(plugin_version)
    set_license("GPL-3.0-only")
    set_pcxxheader("src/PCH.h")

    add_deps("imgui")
	add_packages("nlohmann_json")
    add_defines("_SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING")
    add_syslinks("comctl32")
    add_files("src/**.cpp")
    add_headerfiles("src/**.h", "include/**.h")
    add_includedirs("src", "include")
    add_installfiles(
        "public/SFSE/Plugins/SFSEMenuFrameworkThemes/*.json",
        { prefixdir = "SFSE/Plugins/SFSEMenuFrameworkThemes" }
    )
    add_installfiles(
        "public/SFSE/Plugins/Fonts/*",
        { prefixdir = "SFSE/Plugins/Fonts" }
    )
    add_installfiles("COPYING", "EXCEPTIONS", "THIRD_PARTY_NOTICES.md")

    before_install(function(target)
        target:remove("installfiles", target:symbolfile())
    end)
end)
