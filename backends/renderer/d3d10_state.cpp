// D3D10 state save/restore for StatePolicy::restore. ID3D10Device is both
// device and immediate context; D3D10 has no class instances or UAVs.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "d3d10_state.hpp"

namespace wui::d3d10::detail {

void save_state(ID3D10Device* device, Saved& out) {
    out = Saved{};
    device->IAGetInputLayout(out.layout.GetAddressOf());
    device->IAGetPrimitiveTopology(&out.topology);
    device->IAGetVertexBuffers(
        0, 1, out.vertex_buffer.GetAddressOf(), &out.vertex_stride, &out.vertex_offset);
    device->IAGetIndexBuffer(
        out.index_buffer.GetAddressOf(), &out.index_format, &out.index_offset);
    device->RSGetState(out.rasterizer.GetAddressOf());
    out.viewport_count = static_cast<UINT>(out.viewports.size());
    device->RSGetViewports(&out.viewport_count, out.viewports.data());
    out.scissor_count = static_cast<UINT>(out.scissors.size());
    device->RSGetScissorRects(&out.scissor_count, out.scissors.data());
    device->OMGetBlendState(
        out.blend.GetAddressOf(), out.blend_factor.data(), &out.blend_mask);
    device->OMGetDepthStencilState(
        out.depth_stencil.GetAddressOf(), &out.stencil_ref);
    {
        // Every render-target slot is captured: binding one UI target
        // unbinds the host's other slots.
        ID3D10RenderTargetView* rtvs[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D10DepthStencilView* dsv = nullptr;
        device->OMGetRenderTargets(D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        for (UINT i = 0; i < D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            out.rtvs[i].Attach(rtvs[i]);
        }
        out.dsv.Attach(dsv);
    }
    device->VSGetShader(out.vs.GetAddressOf());
    device->GSGetShader(out.gs.GetAddressOf());
    device->PSGetShader(out.ps.GetAddressOf());
    device->VSGetConstantBuffers(0, 1, out.vs_cbuf0.GetAddressOf());
    device->PSGetConstantBuffers(1, 1, out.ps_cbuf1.GetAddressOf());
    device->PSGetSamplers(0, 1, out.ps_sampler0.GetAddressOf());
    device->PSGetShaderResources(0, 1, out.ps_srv0.GetAddressOf());
}

void restore_state(ID3D10Device* device, const Saved& saved) {
    // Unbind our SRV before restoring output targets so a render-target
    // resource is never simultaneously bound for sampled read.
    ID3D10ShaderResourceView* null_srv = nullptr;
    device->PSSetShaderResources(0, 1, &null_srv);
    {
        ID3D10RenderTargetView* rtvs[D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        for (UINT i = 0; i < D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            rtvs[i] = saved.rtvs[i].Get();
        }
        device->OMSetRenderTargets(
            D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, saved.dsv.Get());
    }
    device->IASetInputLayout(saved.layout.Get());
    device->IASetPrimitiveTopology(saved.topology);
    ID3D10Buffer* vertex_buffer = saved.vertex_buffer.Get();
    device->IASetVertexBuffers(
        0, 1, &vertex_buffer, &saved.vertex_stride, &saved.vertex_offset);
    device->IASetIndexBuffer(
        saved.index_buffer.Get(), saved.index_format, saved.index_offset);
    device->RSSetState(saved.rasterizer.Get());
    // A zero count unbinds every viewport and scissor, matching a host that
    // had none bound.
    device->RSSetViewports(saved.viewport_count,
        saved.viewport_count != 0 ? saved.viewports.data() : nullptr);
    device->RSSetScissorRects(saved.scissor_count,
        saved.scissor_count != 0 ? saved.scissors.data() : nullptr);
    device->OMSetBlendState(
        saved.blend.Get(), saved.blend_factor.data(), saved.blend_mask);
    device->OMSetDepthStencilState(saved.depth_stencil.Get(), saved.stencil_ref);
    device->VSSetShader(saved.vs.Get());
    device->GSSetShader(saved.gs.Get());
    device->PSSetShader(saved.ps.Get());
    ID3D10Buffer* vs_cbuf = saved.vs_cbuf0.Get();
    device->VSSetConstantBuffers(0, 1, &vs_cbuf);
    ID3D10Buffer* ps_cbuf = saved.ps_cbuf1.Get();
    device->PSSetConstantBuffers(1, 1, &ps_cbuf);
    ID3D10SamplerState* sampler = saved.ps_sampler0.Get();
    device->PSSetSamplers(0, 1, &sampler);
    ID3D10ShaderResourceView* srv = saved.ps_srv0.Get();
    device->PSSetShaderResources(0, 1, &srv);
}

} // namespace wui::d3d10::detail
