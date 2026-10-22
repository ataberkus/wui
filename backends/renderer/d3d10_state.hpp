#pragma once

// Private to wui_d3d10. Saved captures every pipeline state render() changes
// so StatePolicy::restore can reinstate host state on success and on every
// failure after capture. Companion to d3d10_state.cpp; included only by the
// renderer implementation and its tests.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d10.h>
#include <wrl/client.h>

#include <array>

namespace wui::d3d10::detail {

using Microsoft::WRL::ComPtr;

struct Saved {
    ComPtr<ID3D10InputLayout> layout;
    D3D10_PRIMITIVE_TOPOLOGY topology = D3D10_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ComPtr<ID3D10Buffer> vertex_buffer;
    UINT vertex_stride = 0, vertex_offset = 0;
    ComPtr<ID3D10Buffer> index_buffer;
    DXGI_FORMAT index_format = DXGI_FORMAT_UNKNOWN;
    UINT index_offset = 0;
    ComPtr<ID3D10RasterizerState> rasterizer;
    UINT viewport_count = 0;
    std::array<D3D10_VIEWPORT, D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
    UINT scissor_count = 0;
    std::array<D3D10_RECT, D3D10_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors{};
    ComPtr<ID3D10BlendState> blend;
    std::array<FLOAT, 4> blend_factor{0, 0, 0, 0};
    UINT blend_mask = 0;
    ComPtr<ID3D10DepthStencilState> depth_stencil;
    UINT stencil_ref = 0;
    std::array<ComPtr<ID3D10RenderTargetView>, D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs;
    ComPtr<ID3D10DepthStencilView> dsv;
    ComPtr<ID3D10VertexShader> vs;
    ComPtr<ID3D10GeometryShader> gs;
    ComPtr<ID3D10PixelShader> ps;
    ComPtr<ID3D10Buffer> vs_cbuf0;
    ComPtr<ID3D10Buffer> ps_cbuf1;
    ComPtr<ID3D10SamplerState> ps_sampler0;
    ComPtr<ID3D10ShaderResourceView> ps_srv0;
};

// Captures the states listed above. Never fails; Get calls on a valid
// device succeed. Callers pass a Saved to fill; prior contents are dropped.
void save_state(ID3D10Device* device, Saved& out);

// Restores a previously saved snapshot.
void restore_state(ID3D10Device* device, const Saved& saved);

} // namespace wui::d3d10::detail
