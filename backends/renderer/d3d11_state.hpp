#pragma once

// Private to wui_d3d11. Saved captures every pipeline state render() changes
// so StatePolicy::restore can reinstate host state on success and on every
// failure after capture. Companion to d3d11_state.cpp; included only by the
// renderer implementation.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <vector>

namespace wui::d3d11::detail {

using Microsoft::WRL::ComPtr;

struct Saved {
    ComPtr<ID3D11InputLayout> layout;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ComPtr<ID3D11Buffer> vertex_buffer;
    UINT vertex_stride = 0, vertex_offset = 0;
    ComPtr<ID3D11Buffer> index_buffer;
    DXGI_FORMAT index_format = DXGI_FORMAT_UNKNOWN;
    UINT index_offset = 0;
    ComPtr<ID3D11RasterizerState> rasterizer;
    UINT viewport_count = 0;
    std::array<D3D11_VIEWPORT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> viewports{};
    UINT scissor_count = 0;
    std::array<D3D11_RECT, D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE> scissors{};
    ComPtr<ID3D11BlendState> blend;
    std::array<FLOAT, 4> blend_factor{0, 0, 0, 0};
    UINT blend_mask = 0;
    ComPtr<ID3D11DepthStencilState> depth_stencil;
    UINT stencil_ref = 0;
    std::array<ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs;
    ComPtr<ID3D11DepthStencilView> dsv;
    // Output-merger UAV bindings observed alongside the render targets so an
    // OM hazard restore preserves append/consume counters via UINT(-1).
    UINT om_uav_count = 0;
    std::array<ComPtr<ID3D11UnorderedAccessView>, D3D11_PS_CS_UAV_REGISTER_COUNT> om_uavs{};
    ComPtr<ID3D11VertexShader> vs;
    std::vector<ComPtr<ID3D11ClassInstance>> vs_instances;
    ComPtr<ID3D11PixelShader> ps;
    std::vector<ComPtr<ID3D11ClassInstance>> ps_instances;
    ComPtr<ID3D11HullShader> hs;
    std::vector<ComPtr<ID3D11ClassInstance>> hs_instances;
    ComPtr<ID3D11DomainShader> ds;
    std::vector<ComPtr<ID3D11ClassInstance>> ds_instances;
    ComPtr<ID3D11GeometryShader> gs;
    std::vector<ComPtr<ID3D11ClassInstance>> gs_instances;
    ComPtr<ID3D11Buffer> vs_cbuf0;
    ComPtr<ID3D11Buffer> ps_cbuf1;
    ComPtr<ID3D11SamplerState> ps_sampler0;
    ComPtr<ID3D11ShaderResourceView> ps_srv0;
    UINT cs_uav_count = 0;
    std::array<ComPtr<ID3D11UnorderedAccessView>, D3D11_PS_CS_UAV_REGISTER_COUNT> cs_uavs{};
};

// Captures the states listed above. Never fails; Get calls on a valid
// immediate context succeed. Callers pass a default Saved to fill.
void save_state(ID3D11DeviceContext* context, Saved& out);

// Restores a previously saved snapshot. UAV bindings use preserved counters.
void restore_state(ID3D11DeviceContext* context, const Saved& saved);

} // namespace wui::d3d11::detail
