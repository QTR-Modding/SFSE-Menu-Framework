package("freetype")
    set_homepage("https://freetype.org")
    set_description("A freely available software library to render fonts.")
    set_license("FTL")

    add_urls(
        "https://downloads.sourceforge.net/project/freetype/freetype2/$(version)/freetype-$(version).tar.gz",
        "https://download.savannah.gnu.org/releases/freetype/freetype-$(version).tar.gz"
    )
    add_versions(
        "2.14.3",
        "e61b31ab26358b946e767ed7eb7f4bb2e507da1cfefeb7a8861ace7fd5c899a1"
    )

    add_configs("shared", {
        description = "Build a shared FreeType library.",
        default = false,
        type = "boolean"
    })
    add_deps("cmake")
    add_includedirs("include/freetype2")

    on_install(function (package)
        local configs = {
            "-DCMAKE_INSTALL_LIBDIR=lib",
            "-DCMAKE_BUILD_TYPE=" .. (package:debug() and "Debug" or "Release"),
            "-DBUILD_SHARED_LIBS=" .. (package:config("shared") and "ON" or "OFF"),
            "-DFT_DISABLE_ZLIB=ON",
            "-DFT_DISABLE_BZIP2=ON",
            "-DFT_DISABLE_PNG=ON",
            "-DFT_DISABLE_BROTLI=ON",
            "-DFT_DISABLE_HARFBUZZ=ON"
        }
        import("package.tools.cmake").install(package, configs)
    end)

    on_test(function (package)
        assert(package:has_cfuncs("FT_Init_FreeType", {
            includes = {
                "ft2build.h",
                "freetype/freetype.h"
            }
        }))
    end)
