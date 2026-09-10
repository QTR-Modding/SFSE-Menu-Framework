#include "appearance/FontManager.h"
#include <imgui.h>
#include <cmath>
#include <cstdlib>
#include <iostream>

namespace Fonts = SFSEMenuFramework::FontManager;
namespace Settings = SFSEMenuFramework::FrameworkSettings;

namespace {
    void Check(bool condition, const char* message)
    {
        if (!condition) { std::cerr << message << '\n'; std::exit(1); }
    }

    bool Upload(const unsigned char* pixels, int width, int height,
        Fonts::TextureBuildResult& result, void*) noexcept
    {
        if (!pixels || width <= 0 || height <= 0) return false;
        result.TextureID = 1;
        return true;
    }

    void Resize(ImGuiIO& io, float width, float height)
    {
        io.DisplaySize = {width, height};
        for (int frame = 0; frame < 20; ++frame) Fonts::UpdateResolutionScale(io);
    }

    void CheckScale(float userScale, float effectiveScale)
    {
        const auto active = Fonts::GetActiveInfo();
        Check(std::abs(active.Settings.UIScale - userScale) < 0.001F, "user scale must not be rewritten");
        Check(std::abs(active.EffectiveUIScale - effectiveScale) < 0.001F, "wrong effective scale");
        Check(std::abs(active.RasterSize - 40.0F * effectiveScale) < 0.001F, "wrong raster size");
        // Dear ImGui rounds requested font sizes down to whole raster pixels.
        Check(ImGui::GetIO().FontDefault->FontSize == std::trunc(active.RasterSize),
            "actual atlas font size must match effective raster size");
        Check(ImGui::GetIO().FontGlobalScale == 1.0F, "fonts must be rebuilt, not bitmap-scaled");
    }
}

int main()
{
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DeltaTime = 1.0F / 60.0F;
    io.DisplaySize = {3840, 2160};
    Check(Fonts::BuildDefaultAtlas(io), "initial atlas build");
    CheckScale(1, 1);
    auto* stableAtlas = io.Fonts;
    const auto apply = [&] {
        Check(Fonts::ApplyPendingAtlas(io, Upload, nullptr) == Fonts::LiveApplyResult::Applied,
            "resolution rebuild must succeed");
        Check(io.Fonts == stableAtlas, "atlas object address must stay stable");
    };
    io.DisplaySize = {2560, 1440};
    Fonts::UpdateResolutionScale(io);
    Check(!Fonts::HasPendingAtlasRebuild(), "resize must settle before rebuilding");
    Resize(io, 2560, 1440);
    apply();
    CheckScale(1, 2.0F / 3.0F);
    Resize(io, 3440, 1440);
    Check(!Fonts::HasPendingAtlasRebuild(), "ultrawide width alone must not enlarge text");
    Resize(io, 1920, 1080);
    apply();
    CheckScale(1, 0.5F);

    auto settings = Fonts::GetActiveInfo().Settings;
    settings.UIScale = 1.25F;
    Check(Fonts::RequestAtlasRebuild(settings), "user scale request");
    Resize(io, 2560, 1440);
    apply();
    CheckScale(1.25F, 1.25F * 2.0F / 3.0F);
    Check(Settings::GetFontSettings().UIScale == 1, "live resolution change must not save settings");

    Resize(io, 3840, 2160);
    auto* previousFont = io.FontDefault;
    Check(Fonts::ApplyPendingAtlas(io, nullptr, nullptr) == Fonts::LiveApplyResult::Failed,
        "failed upload must be reported");
    Check(io.FontDefault == previousFont, "failed upload must retain the old font");
    CheckScale(1.25F, 1.25F * 2.0F / 3.0F);
    Resize(io, 3840, 2160);
    Check(!Fonts::HasPendingAtlasRebuild(), "failed resolution must not retry every frame");
    Check(Fonts::RequestAtlasRebuild(settings), "explicit retry");
    apply();
    CheckScale(1.25F, 1.25F);
    Resize(io, 0, 0);
    Check(!Fonts::HasPendingAtlasRebuild(), "minimization must not rebuild");
    for (int iteration = 0; iteration < 4; ++iteration) {
        Resize(io, 1920, 1080); apply(); CheckScale(1.25F, 0.625F);
        Resize(io, 3840, 2160); apply(); CheckScale(1.25F, 1.25F);
    }
    ImGui::DestroyContext();
    std::cout << "Resolution scaling and atlas replacement tests passed\n";
}
