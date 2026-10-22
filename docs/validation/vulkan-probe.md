# Deliverable B — Vulkan 1.1 contract probe

## Environment and commands

Windows 10.0.19045.6466, VS 2022 x64 MSVC 19.44.35226 (toolset 14.44.35207), Windows SDK 10.0.26100.0. The harness selected **NVIDIA GeForce RTX 2080 Ti**, Vulkan device API **1.4.312**, NVIDIA driver **581.42**, graphics queue family **0**, `R8G8B8A8_UNORM`, single-sample offscreen target. It requested Vulkan **1.1** and enabled `VK_LAYER_KHRONOS_validation`, `VK_EXT_debug_utils`, and `VK_EXT_validation_features` synchronization validation. Every native case asserted **zero validation warnings and errors**, including synchronization hazards. The loader/validation layer reported 1.4.309.

No SDK installer or license-acceptance switch was run. The ignored `out/sdk-src/local-sdk` test toolchain was assembled from source-pinned Khronos Vulkan-ValidationLayers/Headers and glslang `vulkan-sdk-1.4.309.0`, with SPIRV-Tools and a Windows Vulkan loader import library; this is **not** an official LunarG SDK installation. `glslangValidator --version` reported glslang **15.2.0**; `spirv-val --version` reported SPIRV-Tools **2025.1**. CMake found Vulkan headers/library **1.4.309**, glslangValidator and spirv-val. Set `VULKAN_SDK` to a usable SDK/toolchain root (and `VK_LAYER_PATH` for the validation-layer manifest); this local run used ignored `out\vulkan-env.cmd` to do that. A standard Vulkan SDK with those tools should satisfy the same CMake inputs; an SDK install was not exercised here.

```bat
call out\vulkan-env.cmd
cmake --preset win-vulkan-debug
cmake --build --preset win-vulkan-debug
ctest --preset win-vulkan-debug --output-on-failure
cmake --preset win-vulkan-release
cmake --build --preset win-vulkan-release
ctest --preset win-vulkan-release --output-on-failure
```

Both complete Debug and Release suites exited 0: **11/11 CTest entries each**. The Debug native executable reported **12/12 cases, 250/250 assertions**. An independent Debug `WUI_BUILD_TEXT=OFF` build compiled both Vulkan test targets and passed **2/2** targeted entries; the renderer itself does not link the text module. Shader builds ran `glslangValidator -V --target-env vulkan1.1`, then the available `spirv-val --target-env vulkan1.1`, and embedded checked little-endian SPIR-V words into generated headers.

## Contract exercised

- Real graphics pipeline and sampled-image descriptors, 32-byte float vertices, 20-byte push constants, ordered `vkCmdDrawIndexed` with nonzero base vertex, positive-height viewport, floor/ceil clipped scissor, straight-alpha conversion exactly once and premultiplied ONE/ONE_MINUS_SRC_ALPHA blending. The host owned the render pass, command-buffer submission, fence waits and readback. Scale 1 and 2, revised checker, fractional clip and empty batch produced validated readbacks. Incompatible render pass/subpass/extent and duplicate recording were rejected without consuming an invalid target.
- `prepare()` staged RGBA8 textures with transfer barriers before the host render pass. The harness explicitly transitioned its color attachment for transfer readback and synchronized subsequent submissions sharing that attachment/readback buffer. Three real submitted tickets held separate revision images and packet leases; a fourth reuse was `busy`. A deterministic injected `VK_NOT_READY` fence result checked nonblocking retention without making a timing claim about how quickly this GPU completed. After host fence waits, `collect()` retired slot-owned descriptors/resources, and revision 1 rendered again. Recorded and unrecorded tickets were abandoned only after the host discarded their command buffers.
- The real A10 gallery used Inter fonts and an atlas revision forced by `frame.text("Yeni glif: ğ")`; the old image rendered identically after the update and after renderer reconstruction on the same healthy device. An injected `VK_ERROR_DEVICE_LOST` fence query put the renderer in terminal `device_lost` state; this is **simulated error handling**, not physical device loss.

Native staging and readback allocations selected **host-coherent** memory
(`staging_noncoherent=no`, `readback_noncoherent=no` in harness output).
Noncoherent memory-type *selection* had a CPU regression test. The native
flush/invalidate branch was not exercised on the selected compatible types.
External `vulkaninfo --summary` emitted an unrelated implicit RTSS-layer
API-1.3 warning when that utility requested API 1.4; the probe requested
1.1 and its validation callback recorded no warning/error messages.

The raw PPM readbacks remain in ignored `out/win-vulkan-debug/`; linked PNGs are lossless conversions. The gallery images are a **256×256 crop** of the real A10 widget construction, including the visible new glyph; no full-window presentation is claimed:

| Fixture rev 1 | Rev 2 | Scale 2 | Gallery old | Gallery new glyph |
| --- | --- | --- | --- | --- |
| [PNG](b-vulkan-rev1.png) | [PNG](b-vulkan-rev2.png) | [PNG](b-vulkan-scale2.png) | [PNG](b-vulkan-gallery-old.png) | [PNG](b-vulkan-gallery-new.png) |

The two scale-1 fixture images matched the D3D9 HAL images **byte-for-byte over all 49,152 RGB channel values** per image; A9's D3D11 WARP reference differed by at most **1** channel value. Representative captures were visually inspected. Vulkan device loss, a noncoherent native memory type, desktop presentation, and the official SDK installer remain unexercised.
