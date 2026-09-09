-- Keep the pinned MIT-licensed ImGui checkout untouched.
-- Only the compiled copy receives framework-owned interaction notifications.
function main(target)
    local source = io.readfile("extern/imgui/imgui_widgets.cpp"):gsub("\r\n", "\n")
    local function replace_once(needle, replacement)
        local first, last = source:find(needle, 1, true)
        assert(first and not source:find(needle, last + 1, true), "ImGui audio hook needs review: source changed")
        source = source:sub(1, first - 1) .. replacement .. source:sub(last + 1)
    end
    replace_once(
        "    return pressed;\n}\n\nbool ImGui::ButtonEx(",
        "    if (pressed) ImGuiAudio::Notify(ImGuiAudio::Action::Activate);\n    return pressed;\n}\n\nbool ImGui::ButtonEx(")
    replace_once(
        "        *v = !(*v);\n        MarkItemEdited(id);",
        "        *v = !(*v);\n        MarkItemEdited(id);\n        ImGuiAudio::Notify(*v ? ImGuiAudio::Action::ToggleOn : ImGuiAudio::Action::ToggleOff);")
    source = '#include "WidgetAudio.h"\n' .. source
    local generated = path.join(target:autogendir(), "imgui_widgets_audio.cpp")
    if not os.isfile(generated) or io.readfile(generated) ~= source then
        io.writefile(generated, source)
    end
    target:add("files", generated)
end
