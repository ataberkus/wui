#include "d3d9_state.hpp"

#include <algorithm>

namespace wui::d3d9::detail {

Status save_state(IDirect3DDevice9* device, UINT target_count, Saved& saved) {
    saved.target_count = std::clamp(target_count, 1u, 4u);
    if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &saved.block))) {
        return Error{ErrorCode::backend_failure, "D3D9 CreateStateBlock(D3DSBT_ALL) failed"};
    }
    for (UINT i = 0; i < saved.target_count; ++i) {
        const auto hr = device->GetRenderTarget(i, &saved.targets[i]);
        if (FAILED(hr) && (i == 0 || hr != D3DERR_NOTFOUND)) {
            return Error{ErrorCode::backend_failure, "D3D9 GetRenderTarget failed"};
        }
    }
    const auto depth = device->GetDepthStencilSurface(&saved.depth);
    if (FAILED(depth) && depth != D3DERR_NOTFOUND) {
        return Error{ErrorCode::backend_failure, "D3D9 GetDepthStencilSurface failed"};
    }
    if (FAILED(device->GetViewport(&saved.viewport)) ||
        FAILED(device->GetScissorRect(&saved.scissor)) ||
        FAILED(device->GetTransform(D3DTS_WORLD, &saved.world)) ||
        FAILED(device->GetTransform(D3DTS_VIEW, &saved.view)) ||
        FAILED(device->GetTransform(D3DTS_PROJECTION, &saved.projection))) {
        return Error{ErrorCode::backend_failure, "D3D9 viewport/scissor/transform capture failed"};
    }
    return std::nullopt;
}

void restore_state(IDirect3DDevice9* device, const Saved& saved) noexcept {
    // State blocks omit targets, depth and viewport/scissor on D3D9. Setting
    // render targets may reset the viewport, so restore explicit state last.
    saved.block->Apply();
    device->SetRenderTarget(0, saved.targets[0].Get());
    for (UINT i = 1; i < saved.target_count; ++i) {
        device->SetRenderTarget(i, saved.targets[i].Get());
    }
    device->SetDepthStencilSurface(saved.depth.Get());
    device->SetTransform(D3DTS_WORLD, &saved.world);
    device->SetTransform(D3DTS_VIEW, &saved.view);
    device->SetTransform(D3DTS_PROJECTION, &saved.projection);
    device->SetViewport(&saved.viewport);
    device->SetScissorRect(&saved.scissor);
}

} // namespace wui::d3d9::detail
