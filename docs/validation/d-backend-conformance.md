# D: five-backend conformance

Phase D runs the same UI source and the same replayable scenarios through
all five renderers (D3D9, D3D10, D3D11, D3D12, Vulkan) and compares the
images against one reference. This file records what was run, what was
found and fixed, and what was not exercised.

## Environment

| Item | Value |
|---|---|
| OS | Windows 10 Pro 22H2, build 19045, x64 |
| Compiler | MSVC 19.44.35226, Ninja, CMake presets |
| Windows SDK | 10.0.26100 (`d3d10_1.h`, `d3d12.h`, `d3dcompiler_47`; SDK layers for D3D10/11/12 present) |
| Vulkan | loader and `VK_LAYER_KHRONOS_validation` 1.4.309 from the locally built SDK under `out/sdk-src/local-sdk` (via `out/vulkan-env.cmd`), not an official LunarG install |
| GPU 0 | NVIDIA GeForce RTX 2080 Ti (vendor 0x10DE, device 0x1E07) |
| GPU 1 | AMD Radeon(TM) Graphics (used only by the native `wui-d3d12` tests) |
| Software | Microsoft Basic Render Driver (WARP), vendor 0x1414 device 0x008C |

## What the suite does

`tests/conformance/conformance.cpp` builds one executable per renderer
(`wui_conformance_<api>`); a small test-only adapter per API
(`tests/conformance/<api>_backend.cpp`) owns the device, target,
submission and readback the way a host would. Every run:

- renders 88 cases into a cleared single-sample RGBA8 target and writes a
  PPM per case plus `manifest.txt` and `summary.txt`:
  - 7 fixtures (reference packet rev 1/2, scale 2, 1.5x fractional clip,
    empty packet, straight vs premultiplied alpha from a separate
    `TextureStore`, 140,800-vertex packet), each with known-pixel probes;
  - 3 gallery sections at 1280x720 and the 100 / 1,000 / 10,000-row stress
    workloads;
  - 75 checkpoints from `replay_gallery` (four scales, scenarios 01-10,
    resize during press, narrow clipping, focus loss while captured);
- checks lifetime: frames in flight until `busy`, collect, revision and
  glyph-atlas re-renders, a held packet, then device recreate (D3D9: a
  real `Reset`) and byte-identical re-renders;
- counts debug-layer warnings and errors after the case, lifetime and
  recreate groups.

`tools/compare_conformance.py` then compares every run with D3D11 WARP.
Tolerances (fixed in the plan before any run): `fixture/*` max channel
difference <= 2; every other case <= 0.2% of pixels over 8 and max <= 48.

## Results

`win-all-release` and `win-all-debug`, 2026-10-03, after all fixes below:

| Run | Adapter | Debug layer | Suite | Identical | Within tolerance | Failed | Worst |
|---|---|---|---|---|---|---|---|
| d3d11-warp (reference) | WARP, FL 11_0 | D3D11 SDK layer, 0 messages | PASS | - | - | - | - |
| d3d11-hardware | RTX 2080 Ti, FL 11_0 | D3D11 SDK layer, 0 messages | PASS | 2 | 86 | 0 | max 2, 0 px over 8 |
| d3d10-warp | WARP, FL 10_0 | D3D10 SDK layer, 0 messages | PASS | 88 | 0 | 0 | identical |
| d3d10-hardware | RTX 2080 Ti, FL 10_0 | D3D10 SDK layer, 0 messages | PASS | 2 | 86 | 0 | max 2, 0 px over 8 |
| d3d9-hardware | RTX 2080 Ti, HAL | **unavailable** (no D3D9 debug runtime on Windows 10) | PASS | 2 | 86 | 0 | max 2, 0 px over 8 |
| vulkan-hardware | RTX 2080 Ti, API 1.4.351 | Khronos validation + sync validation, 0 messages | PASS | 2 | 86 | 0 | max 2, 0 px over 8 |
| d3d12-warp | WARP, FL 11_0 | D3D12 debug layer (+ GPU-based validation in Debug), 0 messages | PASS | 88 | 0 | 0 | identical |
| d3d12-hardware | RTX 2080 Ti, FL 11_0 | D3D12 debug layer (+ GPU-based validation in Debug), 0 messages | PASS | 2 | 86 | 0 | max 2, 0 px over 8 |

All three WARP runs (D3D10, D3D11, D3D12) produce byte-identical images.
Every hardware run differs from WARP by at most 2 per channel, with no
pixel over 8. Per-case numbers for every backend:
[`d-conformance-cases.md`](d-conformance-cases.md).

Lifetime, on every run: 3 submissions accepted, then `busy`; after
collect, revision 1/2, old and new glyph atlas, and the held packet all
re-render identically; after recreate, `fixture/rev1`, both atlas images
and `stress/controls1000` re-render identically.

The D3D9 run logs its three debug checks as `SKIP` ("not a clean-output
proof"), never `PASS`.

Final matrix on `8616605`: all sixteen CTest presets (core, text, Win32,
each single-backend preset and `win-all-*`, Debug and Release) configure,
build and pass, with `-E wui-win32-clipboard` throughout. The elevated
clipboard suite was not run. Logs: `out/d-final/`.

## Defects the suite found

The first passing run was not trusted: the images were inspected, and the
suite was then strengthened so each defect below now fails automatically.

1. **Texture cache keyed by handle only (D3D11, D3D10, D3D12).** Native
   textures were cached by (slot, generation, revision). Snapshots from
   different `TextureStore`s with equal handles sampled each other's
   pixels (`fixture/alpha-modes` drew white/grey instead of half red; icons
   were wrong in `stress/controls1000`). All three now match snapshot
   identity, extent and alpha mode, as D3D9 already did; each has a
   red-first regression case. The native gallery uses one store, so the
   C4 evidence was unaffected. Vulkan uploads per submission slot and has
   no cross-packet cache.
2. **Scissor rounding at fractional scales (D3D9, Vulkan, D3D12).** Clip
   edges were computed in `double` there and in `float` in D3D10/11, so an
   edge such as `49.6f * 1.25 = 61.9999981` floored into a row of
   anti-aliasing fringe (max diff 109 in 26 checkpoints at 125%/150%).
   All five renderers now share `backends/renderer/scissor.hpp`, which
   snaps edges within 1/1024 px to the pixel boundary before rounding
   outward (unit test `tests/renderers/scissor_test.cpp`, plus backend
   regression cases for Vulkan and D3D12).
3. **D3D12 readback mapping in the test harness.** The native test and the
   adapter mapped `RowPitch * height` bytes; the last row is not padded, so
   `Map` failed whenever the width was not a multiple of 64. Both now map
   the `GetCopyableFootprints` total.

Plan items completed alongside: Vulkan `Config::force_explicit_flush` with
`nonCoherentAtomSize`-aligned ranges, and render-pass compatibility (a
second compatible pass is accepted; a format or sample-count mismatch
returns `unsupported` without consuming the ticket). The suspected D3D9
problem areas (chunking of large packets, the half-pixel offset at
125%/150%) were checked by the suite and were not defects; the 125%/150%
failure was the scissor issue above.

## Allocation regression

After the renderer changes, the D3D11 Release `--measure` gallery gate was
rerun on WARP and the RTX 2080 Ti: `PASS gallery zero-new 300/300` in all
five batches (1280x720 and scales 100/125/150/200) on both. The first
hardware attempt failed with five interference discards (window input or
occlusion, not allocations); the rerun discarded one attempt and passed.

## Simulated or not exercised

- **Device loss is simulated only**, where it is tested at all (D3D12
  completed-value override, Vulkan fence-status override, D3D9 `Reset`
  gating). Physical device removal was never provoked.
- **D3D9 debug runtime**: unavailable on Windows 10; D3D9 output is not
  proven debug-clean.
- **Official Vulkan SDK install**: not used; the layer came from the
  locally built SDK. One loader notice about a duplicate OBS implicit layer
  (`VK_LAYER_WEOBS_HOOK`) is printed but not counted, because it comes from
  this machine's layer manifests, not the renderer.
- **Vulkan noncoherent memory**: the only GPU has coherent upload and
  readback memory. The conformance run forces explicit flushes; the
  default coherent path is covered by `wui-vulkan`.
- **Hardware coverage**: one discrete GPU per API (RTX 2080 Ti). The AMD
  iGPU ran only the native `wui-d3d12` tests. D3D9 has no software
  rasterizer, so it ran on HAL only.
- **D3D12 recreate** gets a new device only after every reference to the
  old one is released (`D3D12CreateDevice` otherwise returns the same
  device; the per-device factory needs the Agility SDK). A private-data
  marker proves the device is new.
- **Not covered by any run**: MSAA targets, BGRA8 targets on D3D10/D3D12,
  separate D3D12 upload/draw lists or multiple queues.
- **Replay drift**: `scenario_replay.cpp` is a line-by-line port of the
  Win32-interleaved scenarios in `examples/widget_gallery/validation.cpp`,
  not a shared implementation. A seeded mutation confirmed the replay test
  fails, but the two can drift if one is edited alone.

## Visual review

Contact sheets in `docs/validation/d-review/`:

- `five-backends-controls.png`: the Controls section at 1280x720 from
  D3D11 WARP and every hardware backend side by side.
- `five-backends-scale150.png`: the same comparison at 150% scale (the
  scale where the scissor defect showed).
- `<run>.png`: per backend, the worst case plus `scale-150/normal-checked`
  and `stress/controls1000`, as reference | candidate | difference (x8,
  red where over 8).

User verdict (2026-10-03, verbatim): "Approved".

## Commands

```bat
call out\vulkan-env.cmd
cmake --preset win-all-release
cmake --build --preset win-all-release
ctest --preset win-all-release -E wui-win32-clipboard
python tools\compare_conformance.py --reference out\win-all-release\conformance\d3d11-warp ^
  --candidate out\win-all-release\conformance\d3d11-hardware ^
  --candidate out\win-all-release\conformance\d3d10-warp ^
  --candidate out\win-all-release\conformance\d3d10-hardware ^
  --candidate out\win-all-release\conformance\d3d9-hardware ^
  --candidate out\win-all-release\conformance\vulkan-hardware ^
  --candidate out\win-all-release\conformance\d3d12-warp ^
  --candidate out\win-all-release\conformance\d3d12-hardware
python tools\conformance_contact_sheet.py --reference out\win-all-release\conformance\d3d11-warp ^
  --candidate out\win-all-release\conformance\d3d9-hardware --case scenario/scale-150/normal-checked ^
  --case stress/controls1000 --out docs\validation\d-review --zoom 1
out\win-d3d11-release\wui_widget_gallery.exe --driver warp --measure out\measure\d\release-warp
out\win-d3d11-release\wui_widget_gallery.exe --driver hardware --measure out\measure\d\release-hardware
```

`wui-conformance-compare` runs the comparison automatically in the
`win-all-*` presets, which also enable the hardware runs
(`WUI_CONFORMANCE_HARDWARE=ON`); single-backend presets run the WARP
(or, for D3D9 and Vulkan, hardware) suite only.
