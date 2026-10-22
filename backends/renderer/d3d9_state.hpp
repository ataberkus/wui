#pragma once

#include <wui/backends/d3d9.hpp>
#include <wrl/client.h>

#include <array>

namespace wui::d3d9::detail {

struct Saved {
    Microsoft::WRL::ComPtr<IDirect3DStateBlock9> block;
    std::array<Microsoft::WRL::ComPtr<IDirect3DSurface9>, 4> targets;
    Microsoft::WRL::ComPtr<IDirect3DSurface9> depth;
    D3DVIEWPORT9 viewport{};
    RECT scissor{};
    D3DMATRIX world{}, view{}, projection{};
    UINT target_count = 1;
};

Status save_state(IDirect3DDevice9* device, UINT target_count, Saved& saved);
void restore_state(IDirect3DDevice9* device, const Saved& saved) noexcept;

} // namespace wui::d3d9::detail
