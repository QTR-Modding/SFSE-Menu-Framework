#pragma once

#include <d3d12.h>
struct ImDrawData;

// Consumes one backend frame slot even on failure; callers must retire its uploads.
bool ImGui_ImplDX12_RenderDrawDataChecked(ImDrawData* data, ID3D12GraphicsCommandList* list,
    D3D12_CPU_DESCRIPTOR_HANDLE target, bool clear);
