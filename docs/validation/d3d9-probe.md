# Deliverable B — D3D9 contract probe

## Environment and commands

Windows 10.0.19045.6466, VS 2022 x64 MSVC 19.44.35226 (toolset 14.44.35207), Windows SDK 10.0.26100.0. The native test created a hidden HWND and a real D3D9 HAL device on **NVIDIA GeForce RTX 2080 Ti**; no WARP/reference-device substitution. Reported caps: VS 3.0, PS 3.0, MaxVertexIndex 16,777,215, MaxPrimitiveCount 16,777,215, MaxTexture 16,384×16,384; alpha, linear min/mag filtering, scissor, ONE/INVSRCALPHA color and separate-alpha blending, `D3DQUERYTYPE_EVENT`, and `A8R8G8B8` upload were available. The test used single-sample offscreen `A8R8G8B8` targets. A legacy D3D9 debug runtime was **not** established; no debug-runtime-clean claim is made.

Run from a VS x64 Developer Prompt with the pinned text dependencies already staged for the corresponding configuration:

```bat
cmake --preset win-d3d9-debug
cmake --build --preset win-d3d9-debug
ctest --preset win-d3d9-debug --output-on-failure
cmake --preset win-d3d9-release
cmake --build --preset win-d3d9-release
ctest --preset win-d3d9-release --output-on-failure
```

Both complete Debug and Release suites exited 0: **11/11 CTest entries each**. The Debug native HAL executable reported **13/13 cases, 603/603 assertions**; the chunk executable reported **7/7 cases, 70,063/70,063 assertions**. A separate Debug build with `WUI_BUILD_TEXT=OFF` compiled both D3D9 test targets and passed **2/2** targeted CTest entries; the renderer does not depend on the optional text module.

## Contract exercised

- Strictly compiled VS/PS 3.0, 32-byte float vertex layout, RGBA→BGRA upload respecting locked pitch, one physical half-pixel correction, ordered textured triangle lists, premultiplied color/alpha blending and clipped scissor. A 70,002-vertex draw crossed the 16-bit local-index boundary and rendered both early and final triangles. Straight and premultiplied texture modes each produced the expected half-alpha red on black.
- Default state isolation restored non-null host vertex/pixel shaders, stream/index/declaration, render targets including an extra MRT where supported, a non-null depth target, viewport/scissor, texture/sampler/blend/cull settings, transforms and shader constants after successful rendering and a late texture-limit failure. `caller_restores` intentionally left the UI target bound.
- Three EVENT-query submissions retained packets and native texture revisions; a fourth render returned `busy`. `collect()` used `GetData(..., 0)` without flushing or spinning, then released the old packet after GPU completion. Checker revision 1 rendered again after revision 2. Separate immutable snapshots sharing handle/revision/dimensions/alpha but containing white versus red pixels rendered white/red/white rather than erroneously reusing one native texture.
- Ten real `IDirect3DDevice9::Reset` cycles alternated 128/256-pixel targets; `before_reset()` gated rendering with `device_lost`, released renderer-owned reset-sensitive objects, and `after_reset()` rebuilt shaders/buffers/textures from retained packets. An additional real Reset followed small→large default-pool buffer growth while the older UI stream/index buffer had been restored to host state. Ten further real resets alternated presentation dimensions while old/new real gallery atlas images stayed byte-identical to their pre-reset captures. The model's `Notifications=false` checkbox and `restore_count=7` survived; renderer destruction preceded device release.

The 128×128 fixture readbacks below were produced by `GetRenderTargetData`; the 640×400 gallery readbacks show the real Inter atlas before/after `frame.text("Yeni glif: ğ")`. Tests wrote raw PPM files in `out/win-d3d9-debug/`; the linked PNGs are lossless conversions of those captures:

| Reference revision 1 | Revision 2 | Gallery before | Gallery with new glyph |
| --- | --- | --- | --- |
| [PNG](b-d3d9-rev1.png) | [PNG](b-d3d9-rev2.png) | [PNG](b-d3d9-gallery-old.png) | [PNG](b-d3d9-gallery-new.png) |

Both fixture revisions matched the Vulkan probe **byte-for-byte across 49,152 RGB channel values** per 128×128 image. Against A9's D3D11 WARP fixture captures, maximum channel difference was **1**; the checker samples at `(76,12)` and `(84,12)` were `(225,225,225)` and `(30,30,30)` in revision 1, accounting for linear filtering. Native readbacks were inspected visually. Healthy-device `Reset` is not proof of physical device loss or a legacy debug runtime; no swap chain was presented to the desktop.
