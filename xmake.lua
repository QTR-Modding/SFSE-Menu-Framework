set_xmakever("3.0.9")
set_policy("package.requires_lock", true)

local project_root = os.projectdir()

if is_plat("windows") then
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

-- This project performs deployment only after verification. Keep all other
-- CommonLib plugin behavior, but replace its implicit post-build install hook.
rule("commonlib.plugin", function()
    after_build(function() end)
end)

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
local plugin_version = "1.0.0"
local plugin_author = "Quantumyilmaz"
local build_staging_dir = path.join(project_root, "build", "staging")
local sdk_root = path.join(project_root, "..", "SFSE-MCP")

set_project(plugin_name)
set_version(plugin_version)
set_license("GPL-3.0-only")
set_languages("c++23")
set_warnings("allextra")
set_encodings("utf-8")

add_rules("mode.debug", "mode.releasedbg", "mode.release")
add_rules("plugin.vsxmake.autoupdate")

target("imgui", function()
    on_load(function(target)
        import("scripts.imgui-audio", { rootdir = project_root })(target)
        import("scripts.imgui-navigation", { rootdir = project_root })(target)
    end)
    set_kind("static")
    set_default(false)
    set_license("MIT")

    add_files(
        "extern/imgui/imgui_demo.cpp",
        "extern/imgui/imgui_tables.cpp",
        "extern/imgui/backends/imgui_impl_win32.cpp",
        "extern/imgui_bridge/FontVariation.cpp",
        "extern/imgui_bridge/imgui_freetype_bridge.cpp"
    )
    add_files("extern/imgui/imgui_draw.cpp", {
        defines = "IMGUI_ENABLE_STB_TRUETYPE"
    })
    add_files("extern/imgui/backends/imgui_impl_dx12.cpp", {
        cxflags = "/wd4189"
    })
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
    add_defines(
        "IMGUI_ENABLE_FREETYPE",
        "IMGUI_IMPL_WIN32_DISABLE_GAMEPAD"
    )
    add_includedirs("extern/imgui", { public = true })
    add_includedirs("extern/imgui_bridge", { public = true })
    add_packages("freetype", { public = true })
    add_syslinks("d3dcompiler", { public = true })
end)

target("verify-framework-signature", function()
    set_kind("binary")
    set_default(false)
    add_files(path.join(sdk_root, "tools", "verify_signature.cpp"))
    add_includedirs(path.join(sdk_root, "include"))
    add_includedirs(path.join(sdk_root, "lib", "clib-utils-qtr", "include"))
end)

target(dll_name, function()
    on_package(function(target)
        os.vrunv("powershell", {
            "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "RemoteSigned", "-File",
            path.join(project_root, "scripts", "Prepare-Release.ps1"),
            "-BuiltDll", target:targetfile(), "-Version", target:version()
        })
    end)
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
    add_deps("verify-framework-signature", { inherit = false })
	add_packages("nlohmann_json")
    add_defines("_SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING")
    add_syslinks("comctl32", "windowscodecs", "ole32", "xaudio2")
    -- Generated from Dear ImGui 1.90.8-docking by cimgui and copied from the
    -- pinned SKSE Menu Framework reference. Compile it directly into the DLL
    -- so every CIMGUI_API entry remains present in the export table.
    add_files("extern/cimgui-generated/cimgui.cpp")
    add_headerfiles("extern/cimgui-generated/cimgui.h")
    add_files("src/**.cpp")
    add_headerfiles("src/**.h")
    add_includedirs("src")
    add_installfiles(
        "public/SFSE/Plugins/SFSEMenuFrameworkThemes/*.json",
        { prefixdir = "SFSE/Plugins/SFSEMenuFrameworkThemes" }
    )
    add_installfiles(
        "public/SFSE/Plugins/SFSEMenuFrameworkThemes/wallpapers/unity.png",
        { prefixdir = "SFSE/Plugins/SFSEMenuFrameworkThemes/wallpapers" }
    )
    add_installfiles(
        "public/SFSE/Plugins/Fonts/*",
        { prefixdir = "SFSE/Plugins/Fonts" }
    )
    add_installfiles("COPYING", "EXCEPTIONS", "THIRD_PARTY_NOTICES.md")

    -- CommonLibSF derives an automatic post-build install destination from
    -- environment variables. Override it with build-local staging so compiling
    -- cannot touch an active game or mod-manager setup. Deployment is explicit.
    -- It also adds the PDB during configuration; keep symbols local for packages.
    on_config(function(target)
        target:set("installdir", build_staging_dir)
        target:remove("installfiles", target:symbolfile())
    end)

    before_build(function(target)
        assert(
            path.absolute(target:installdir()) == path.absolute(build_staging_dir),
            "refusing to build with a non-staging install destination"
        )
    end)
    after_build(function(target)
        os.vrunv("powershell", {
            "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "RemoteSigned", "-File",
            path.join(project_root, "scripts", "signing", "Sign-Build.ps1"),
            "-DllPath", target:targetfile(),
            "-VerifyTool", target:dep("verify-framework-signature"):targetfile()
        })
        if is_mode("release", "releasedbg") then
            os.vrunv("powershell", {
                "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "RemoteSigned", "-File",
                path.join(project_root, "scripts", "Prepare-Release.ps1"),
                "-BuiltDll", target:targetfile(), "-Version", target:version()
            })
        end
    end)
end)

target("menu-path-tests", function()
    set_kind("binary")
    set_default(false)

    add_files(
        "tests/MenuPathTests.cpp",
        "src/runtime/MenuPath.cpp"
    )
    add_includedirs("src")
    add_tests("default")
end)

target("command-list-state-tests", function()
    set_kind("binary")
    set_default(false)
    set_pcxxheader("src/PCH.h")
    add_deps("commonlibsf")
    add_files("tests/CommandListStateTests.cpp", "src/rendering/CommandListState.cpp",
        "src/rendering/RenderHooks.cpp")
    add_includedirs("src")
    add_syslinks("d3d12", "dxgi", "d3dcompiler")
    add_tests("default")
end)

target("theme-cursor-tests", function()
    set_kind("binary")
    set_pcxxheader("src/PCH.h")
    add_deps("commonlibsf")
    set_default(false)
    add_deps("imgui")
    add_packages("nlohmann_json")
    add_syslinks("windowscodecs", "ole32")
    add_files(
        "tests/ThemeCursorTests.cpp",
        "tests/CursorCatalogTests.cpp",
        "src/appearance/CursorManager.cpp",
        "src/config/FrameworkSettings.cpp",
        "src/config/FrameworkSettingsIO.cpp",
        "src/appearance/CursorDrawing.cpp",
        "src/appearance/ThemeImage.cpp"
    )
    add_includedirs("src")
    add_tests("default")
end)

target("sound-wave-tests", function()
    set_kind("binary")
    set_default(false)
    add_files("tests/WaveClipTests.cpp", "src/audio/WaveClip.cpp")
    add_includedirs("src")
    add_tests("default")
end)

target("sound-interaction-tests", function()
    set_kind("binary")
    set_default(false)
    set_targetdir(path.join(project_root, "build", "sound-tests"))
    add_deps("imgui")
    add_files("tests/SoundInteractionTests.cpp", "src/audio/InteractionSounds.cpp",
        "src/config/FrameworkSettings.cpp", "src/config/FrameworkSettingsIO.cpp")
    add_includedirs("src")
    add_tests("default")
end)

target("mcp-gamepad-tests", function()
    set_kind("binary")
    set_default(false)
    add_deps("imgui")
    add_files("tests/McpGamepadTests.cpp", "src/ui/McpGamepad.cpp", "src/appearance/GamepadIcons.cpp")
    add_includedirs("src")
    add_tests("default")
end)

target("gamepad-input-tests", function()
    set_kind("binary")
    set_default(false)
    set_pcxxheader("src/PCH.h")
    add_deps("commonlibsf", "imgui")
    add_files("tests/GamepadInputTests.cpp", "src/input/GamepadNavigation.cpp")
    add_includedirs("src")
    add_tests("default")
end)

target("font-resolution-tests", function()
    set_kind("binary")
    set_default(false)
    set_targetdir(path.join(project_root, "build", "font-tests"))
    set_pcxxheader("src/PCH.h")
    add_deps("commonlibsf", "imgui")
    add_packages("nlohmann_json")
    add_files("tests/FontResolutionTests.cpp", "src/appearance/FontManager.cpp",
        "src/appearance/fonts/*.cpp", "src/config/FrameworkSettings.cpp",
        "src/config/FrameworkSettingsIO.cpp")
    add_includedirs("src")
    add_tests("default")
end)
