-- Adapt Dear ImGui 6d948ab (MIT), keeping its public backend entry point intact.
function main(target)
    local source = io.readfile("extern/imgui/backends/imgui_impl_dx12.cpp"):gsub("\r\n", "\n")
    local signature = "void ImGui_ImplDX12_RenderDrawData(ImDrawData* draw_data, ID3D12GraphicsCommandList* ctx)"
    local first = assert(source:find(signature, 1, true))
    local last = assert(source:find("\nstatic void ImGui_ImplDX12_CreateFontsTexture()", first, true))
    local body = source:sub(first, last - 1)
    local function replace(old, new)
        local a, b = body:find(old, 1, true)
        assert(a and not body:find(old, b + 1, true), "ImGui DX12 source changed; review adapter")
        body = body:sub(1, a - 1) .. new .. body:sub(b + 1)
    end
    replace(signature, "bool ImGui_ImplDX12_RenderDrawDataChecked(ImDrawData* draw_data, ID3D12GraphicsCommandList* ctx, D3D12_CPU_DESCRIPTOR_HANDLE target, bool clear)")
    local minimized = "    // Avoid rendering when minimized\n    if (draw_data->DisplaySize.x <= 0.0f || draw_data->DisplaySize.y <= 0.0f)\n        return;\n\n"
    replace(minimized, "")
    local slot = "    ImGui_ImplDX12_RenderBuffers* fr = &vd->FrameRenderBuffers[vd->FrameIndex % bd->numFramesInFlight];\n"
    replace(slot, slot .. "\n" .. minimized)
    replace("    if (fr->IndexBuffer->Map(0, &range, &idx_resource) != S_OK)\n        return;",
        "    if (fr->IndexBuffer->Map(0, &range, &idx_resource) != S_OK)\n    {\n        fr->VertexBuffer->Unmap(0, &range);\n        return;\n    }")
    replace("    // Setup desired DX state", [[    if (target.ptr)
    {
        const float transparent[4] = {};
        if (clear) ctx->ClearRenderTargetView(target, transparent, 0, nullptr);
        ctx->OMSetRenderTargets(1, &target, FALSE, nullptr);
    }

    // Setup desired DX state]])
    local failures
    body, failures = body:gsub("return;", "return false;")
    assert(failures == 5, "ImGui DX12 failure paths changed")
    body = body:gsub("%s*$", "")
    assert(body:sub(-1) == "}", "ImGui DX12 function boundary changed")
    body = body:sub(1, -2) .. "    return true;\n}\n\n" .. signature .. [[

{
    (void)ImGui_ImplDX12_RenderDrawDataChecked(draw_data, ctx, {}, false);
}
]]
    source = '#include "DX12Draw.h"\n' .. source:sub(1, first - 1) .. body .. source:sub(last)
    local generated = path.join(target:autogendir(), "imgui_dx12.cpp")
    if not os.isfile(generated) or io.readfile(generated) ~= source then io.writefile(generated, source) end
    target:add("files", generated, {cxflags = "/wd4189"})
    target:add("includedirs", "extern/imgui/backends")
end
