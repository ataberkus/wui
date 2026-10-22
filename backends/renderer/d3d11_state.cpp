// A9 D3D11 state save/restore for StatePolicy::restore.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "d3d11_state.hpp"

namespace wui::d3d11::detail {
namespace {
template <class Shader, class Instance, class Getter>
void get_shader_instances(Getter getter, ID3D11DeviceContext* context,
    Microsoft::WRL::ComPtr<Shader>& shader,
    std::vector<Microsoft::WRL::ComPtr<Instance>>& instances) {
    Microsoft::WRL::ComPtr<Shader> held;
    UINT count = 0;
    (context->*getter)(held.GetAddressOf(), nullptr, &count);
    instances.clear();
    if (count != 0) {
        std::vector<Instance*> raw(count, nullptr);
        Microsoft::WRL::ComPtr<Shader> again;
        UINT again_count = count;
        (context->*getter)(again.GetAddressOf(), raw.data(), &again_count);
        held = again;
        instances.reserve(again_count);
        for (UINT i = 0; i < again_count; ++i) {
            Microsoft::WRL::ComPtr<Instance> owned;
            owned.Attach(raw[i]);
            instances.push_back(std::move(owned));
        }
    }
    shader = held;
}
template <class Shader, class Instance, class Setter>
void set_shader_instances(Setter setter, ID3D11DeviceContext* context,
    const Microsoft::WRL::ComPtr<Shader>& shader,
    const std::vector<Microsoft::WRL::ComPtr<Instance>>& instances) {
    std::vector<Instance*> raw;
    raw.reserve(instances.size());
    for (const auto& owned : instances) raw.push_back(owned.Get());
    (context->*setter)(shader.Get(), raw.data(),
        static_cast<UINT>(raw.size()));
}

} // namespace

void save_state(ID3D11DeviceContext* context, Saved& out) {
    out = Saved{};
    context->IAGetInputLayout(out.layout.GetAddressOf());
    context->IAGetPrimitiveTopology(&out.topology);
    context->IAGetVertexBuffers(
        0, 1, out.vertex_buffer.GetAddressOf(), &out.vertex_stride, &out.vertex_offset);
    context->IAGetIndexBuffer(
        out.index_buffer.GetAddressOf(), &out.index_format, &out.index_offset);
    context->RSGetState(out.rasterizer.GetAddressOf());
    out.viewport_count = static_cast<UINT>(out.viewports.size());
    context->RSGetViewports(&out.viewport_count, out.viewports.data());
    out.scissor_count = static_cast<UINT>(out.scissors.size());
    context->RSGetScissorRects(&out.scissor_count, out.scissors.data());
    context->OMGetBlendState(
        out.blend.GetAddressOf(), out.blend_factor.data(), &out.blend_mask);
    context->OMGetDepthStencilState(
        out.depth_stencil.GetAddressOf(), &out.stencil_ref);
    {
        ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* dsv = nullptr;
        context->OMGetRenderTargets(
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, &dsv);
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            out.rtvs[i].Attach(rtvs[i]);
        }
        out.dsv.Attach(dsv);
    }
    {
        // Observe output-merger UAV bindings alongside the render targets.
        // The render-target and depth arrays must be valid: the debug layer
        // access-violates on null arrays even for state queries. Duplicates
        // are released; UAVs transfer into the snapshot.
        ID3D11RenderTargetView* again[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* depth_again = nullptr;
        ID3D11UnorderedAccessView* uavs[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
        context->OMGetRenderTargetsAndUnorderedAccessViews(
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, again, &depth_again,
            0, D3D11_PS_CS_UAV_REGISTER_COUNT, uavs);
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            if (again[i] != nullptr) again[i]->Release();
        }
        if (depth_again != nullptr) depth_again->Release();
        // The API reports the bound count only through the caller's slot
        // range; retain the full slot array so restore covers every binding
        // the renderer could disturb.
        out.om_uav_count = D3D11_PS_CS_UAV_REGISTER_COUNT;
        for (UINT i = 0; i < D3D11_PS_CS_UAV_REGISTER_COUNT; ++i) {
            out.om_uavs[i].Attach(uavs[i]);
        }
    }
    get_shader_instances(&ID3D11DeviceContext::VSGetShader, context, out.vs, out.vs_instances);
    get_shader_instances(&ID3D11DeviceContext::PSGetShader, context, out.ps, out.ps_instances);
    get_shader_instances(&ID3D11DeviceContext::HSGetShader, context, out.hs, out.hs_instances);
    get_shader_instances(&ID3D11DeviceContext::DSGetShader, context, out.ds, out.ds_instances);
    get_shader_instances(&ID3D11DeviceContext::GSGetShader, context, out.gs, out.gs_instances);
    context->VSGetConstantBuffers(0, 1, out.vs_cbuf0.GetAddressOf());
    context->PSGetConstantBuffers(1, 1, out.ps_cbuf1.GetAddressOf());
    context->PSGetSamplers(0, 1, out.ps_sampler0.GetAddressOf());
    context->PSGetShaderResources(0, 1, out.ps_srv0.GetAddressOf());
    {
        ID3D11UnorderedAccessView* uavs[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
        context->CSGetUnorderedAccessViews(
            0, D3D11_PS_CS_UAV_REGISTER_COUNT, uavs);
        out.cs_uav_count = D3D11_PS_CS_UAV_REGISTER_COUNT;
        for (UINT i = 0; i < D3D11_PS_CS_UAV_REGISTER_COUNT; ++i) {
            out.cs_uavs[i].Attach(uavs[i]);
        }
    }
}

void restore_state(ID3D11DeviceContext* context, const Saved& saved) {
    // Unbind our SRV before restoring output targets so a render-target
    // resource is never simultaneously bound for sampled read.
    ID3D11ShaderResourceView* null_srv = nullptr;
    context->PSSetShaderResources(0, 1, &null_srv);
    // Restore output bindings first so later viewport/scissor values are not
    // overwritten by target changes; UAV counters stay preserved.
    {
        ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
            rtvs[i] = saved.rtvs[i].Get();
        }
        // Plain OMSetRenderTargets unbinds output-merger UAVs, and the
        // UAV-variant call drops render targets when its UAV range covers
        // unbound slots with preserved counters. Bind targets plainly, then
        // restore exactly the saved non-null UAV span (initial counts are
        // preserved for bound slots and zero for gaps).
        context->OMSetRenderTargets(
            D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, saved.dsv.Get());
        UINT first = D3D11_PS_CS_UAV_REGISTER_COUNT, last = 0;
        for (UINT i = 0; i < D3D11_PS_CS_UAV_REGISTER_COUNT; ++i) {
            if (saved.om_uavs[i].Get() != nullptr) {
                if (i < first) first = i;
                last = i;
            }
        }
        if (last >= first) {
            ID3D11UnorderedAccessView* span[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
            UINT counts[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
            for (UINT i = first; i <= last; ++i) {
                span[i - first] = saved.om_uavs[i].Get();
                counts[i - first] = span[i - first] != nullptr ? UINT(-1) : 0;
            }
            context->OMSetRenderTargetsAndUnorderedAccessViews(
                D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, rtvs, saved.dsv.Get(),
                first, last - first + 1, span, counts);
        }
    }
    context->IASetInputLayout(saved.layout.Get());
    context->IASetPrimitiveTopology(saved.topology);
    context->IASetVertexBuffers(0, 1, saved.vertex_buffer.GetAddressOf(),
        &saved.vertex_stride, &saved.vertex_offset);
    context->IASetIndexBuffer(
        saved.index_buffer.Get(), saved.index_format, saved.index_offset);
    context->RSSetState(saved.rasterizer.Get());
    if (saved.viewport_count != 0) {
        context->RSSetViewports(saved.viewport_count, saved.viewports.data());
    }
    if (saved.scissor_count != 0) {
        context->RSSetScissorRects(saved.scissor_count, saved.scissors.data());
    }
    context->OMSetBlendState(
        saved.blend.Get(), saved.blend_factor.data(), saved.blend_mask);
    context->OMSetDepthStencilState(saved.depth_stencil.Get(), saved.stencil_ref);
    set_shader_instances(&ID3D11DeviceContext::VSSetShader, context,
        saved.vs, saved.vs_instances);
    set_shader_instances(&ID3D11DeviceContext::PSSetShader, context,
        saved.ps, saved.ps_instances);
    set_shader_instances(&ID3D11DeviceContext::HSSetShader, context,
        saved.hs, saved.hs_instances);
    set_shader_instances(&ID3D11DeviceContext::DSSetShader, context,
        saved.ds, saved.ds_instances);
    set_shader_instances(&ID3D11DeviceContext::GSSetShader, context,
        saved.gs, saved.gs_instances);
    ID3D11Buffer* vs_cbuf = saved.vs_cbuf0.Get();
    context->VSSetConstantBuffers(0, 1, &vs_cbuf);
    ID3D11Buffer* ps_cbuf = saved.ps_cbuf1.Get();
    context->PSSetConstantBuffers(1, 1, &ps_cbuf);
    ID3D11SamplerState* sampler = saved.ps_sampler0.Get();
    context->PSSetSamplers(0, 1, &sampler);
    ID3D11ShaderResourceView* srv = saved.ps_srv0.Get();
    context->PSSetShaderResources(0, 1, &srv);
    {
        bool any_uav = false;
        ID3D11UnorderedAccessView* cs_uavs[D3D11_PS_CS_UAV_REGISTER_COUNT] = {};
        for (UINT i = 0; i < D3D11_PS_CS_UAV_REGISTER_COUNT; ++i) {
            cs_uavs[i] = saved.cs_uavs[i].Get();
            any_uav = any_uav || cs_uavs[i] != nullptr;
        }
        // The renderer never binds compute UAVs; leave host bindings alone
        // when none were saved, mirroring the output-merger rule above.
        if (any_uav) {
            static const UINT keep_counters[D3D11_PS_CS_UAV_REGISTER_COUNT] = {
                UINT(-1), UINT(-1), UINT(-1), UINT(-1),
                UINT(-1), UINT(-1), UINT(-1), UINT(-1)};
            context->CSSetUnorderedAccessViews(
                0, saved.cs_uav_count, cs_uavs, keep_counters);
        }
    }
}

} // namespace wui::d3d11::detail
