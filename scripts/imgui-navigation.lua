-- Port SKSE-MF 8fb2d295's ItemAdd observer into the compiled ImGui source.
-- Keep the pinned checkout and public ImGui ABI unchanged.
function main(target)
    local source = io.readfile("extern/imgui/imgui.cpp"):gsub("\r\n", "\n")
    -- Dear ImGui 6d948ab (MIT) manually scrolls with LStick in NavUpdate.
    -- Our navigation uses LStick for selection and RStick for manual scrolling.
    -- Retain the separate scroll-to-selected-item and empty-window fallback paths.
    local scroll_start = "        // *Normal* Manual scroll with LStick\n"
    local scroll_end = "\n    }\n\n    // Always prioritize mouse highlight"
    local scroll_first = source:find(scroll_start, 1, true)
    local scroll_last = scroll_first and source:find(scroll_end, scroll_first, true)
    assert(scroll_first and scroll_last and not source:find(scroll_start, scroll_first + 1, true),
        "ImGui manual stick scrolling needs review: source changed")
    source = source:sub(1, scroll_first - 1) .. source:sub(scroll_last)
    local needle = "    g.LastItemData.StatusFlags = ImGuiItemStatusFlags_None;"
    local first, last = source:find(needle, 1, true)
    assert(first and not source:find(needle, last + 1, true), "ImGui ItemAdd observer needs review: source changed")
    source = source:sub(1, last) ..
        "\n    if (id != 0 && GItemAddObserver != nullptr)\n        GItemAddObserver(&g, window, &g.LastItemData);" ..
        source:sub(last + 1)
    -- Extend Dear ImGui 6d948ab (MIT) at its shared slider/drag adjustment path.
    local tweak = "    return amount;\n}\n\nstatic void ImGui::NavUpdate()"
    local tweak_first, tweak_last = source:find(tweak, 1, true)
    assert(tweak_first and not source:find(tweak, tweak_last + 1, true),
        "ImGui navigation tweak provider needs review: source changed")
    source = source:sub(1, tweak_first - 1) ..
        "    if (GNavTweakProvider != nullptr)\n        amount += GNavTweakProvider(axis);\n" ..
        source:sub(tweak_first)
    source = '#define IMGUI_DEFINE_MATH_OPERATORS\n#include "ItemObserver.h"\n' ..
        'static ImGui::ImGuiItemAddObserver GItemAddObserver = nullptr;\n' ..
        'static ImGui::ImGuiNavTweakProvider GNavTweakProvider = nullptr;\n' ..
        'void ImGui::SetNavTweakProvider(ImGuiNavTweakProvider provider) { GNavTweakProvider = provider; }\n' ..
        'void ImGui::SetItemAddObserver(ImGuiItemAddObserver observer) { GItemAddObserver = observer; }\n' .. source
    local generated = path.join(target:autogendir(), "imgui_navigation.cpp")
    if not os.isfile(generated) or io.readfile(generated) ~= source then
        io.writefile(generated, source)
    end
    target:add("files", generated)
end
