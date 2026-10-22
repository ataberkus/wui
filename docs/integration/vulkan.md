# WUI Vulkan integration

`wui::vulkan::Renderer` (`include/wui/backends/vulkan.hpp`, target
`wui_vulkan`) records draw packets into command buffers the host owns. It
never gets a queue, submits, resets or waits on a fence, begins a render
pass, creates a swap chain or starts a thread. The tests are
`wui-vulkan-lifetime` (device-free state machine and memory helpers),
`wui-vulkan` (native) and `wui-conformance-vulkan-hardware`, all in the
`win-vulkan-debug` and `win-vulkan-release` presets.

## Build

```bat
call out\vulkan-env.cmd
cmake --preset win-vulkan-debug
cmake --build --preset win-vulkan-debug
ctest --preset win-vulkan-debug
```

`out\vulkan-env.cmd` calls `out\native-env.cmd` (MSVC x64, CMake, Ninja),
sets `VULKAN_SDK` to `out\sdk-src\local-sdk` and `VK_LAYER_PATH` to its
`Bin`, and puts that `Bin` first on `PATH`. This is a locally built SDK, not
an official LunarG install. The presets set `WUI_BUILD_VULKAN=ON` and
inherit `core-text-debug`, so the gallery atlas test and the conformance
suite are built too. Configure needs Vulkan with `glslangValidator` and
Python 3. The GLSL shaders are compiled for `vulkan1.1`, checked with
`spirv-val` when found, and embedded by `tools/embed_spirv.py`. `wui_vulkan`
links `Vulkan::Vulkan` and calls the loader's exported entry points (no
dispatch table). The native tests fail without `VK_LAYER_KHRONOS_validation`
and `VK_EXT_validation_features`.

## Host / renderer split

| Host owns | Renderer owns |
| --- | --- |
| Instance, physical device, device, queue | Descriptor set layout (one combined image sampler), linear clamp-to-edge sampler |
| Render pass, framebuffer, target image, its layouts and synchronization | Pipeline layout (set 0, 20-byte push constants), one pipeline for `render_pass`/`subpass` |
| Command pool and buffers: reset, begin, end | One descriptor pool (256 sets) per frame slot |
| Fences (create, reset, wait), `vkQueueSubmit`, presentation | Per-slot vertex, index and staging buffers and texture images |

## Instance, device and Config

```cpp
wui::vulkan::Config config{physical, device, graphics_family, render_pass};
config.subpass = 0;                        // subpass the pipeline is built for
config.format = VK_FORMAT_R8G8B8A8_UNORM;  // the only accepted format
config.samples = VK_SAMPLE_COUNT_1_BIT;    // the only accepted sample count
config.max_frames = 3;                     // frame slots the host will use
config.force_explicit_flush = false;       // test-only, see Memory
auto created = wui::vulkan::Renderer::create(config);
```

- `invalid_argument`: null physical device, device or render pass, `max_frames == 0`, or `graphics_family` out of range. No upper bound on `max_frames` is checked.
- `unsupported`: any other format or sample count (tested with `B8G8R8A8_UNORM` and 4 samples); a physical device below API 1.1; `R8G8B8A8_UNORM` optimal tiling without color attachment, blend, sampled, linear filter, transfer source and transfer destination support; `samples` missing from `framebufferColorSampleCounts`; or a family without the graphics bit or queues.

The family is only validated; the renderer never gets a queue. No device
extension or feature is required. The tests use an API 1.1 instance and a
device with one queue and nothing enabled. Uploads need a host-visible
memory type, or `prepare` returns `unsupported`; images prefer device-local.

`force_explicit_flush` flushes every mapped upload even on host-coherent
memory, so the noncoherent path runs under the validation layer. It is off
by default and meant for tests. Noncoherent memory is always flushed.

## Target and render-pass compatibility

`Target` is `{render_pass, subpass, extent, format, samples}`; `format` and
`samples` default to RGBA8 and one sample. `render_pass` is the pass the
host has begun. It may differ from `Config::render_pass` if it is render
pass compatible (same attachment formats, sample counts and subpass
structure). The renderer does not inspect the pass: it compares only the
declared `format` and `samples` with `Config`, and a mismatch returns
`unsupported`. A null pass, a `subpass` other than `Config::subpass`, or an
`extent` other than the packet viewport times its scale (rounded) returns
`invalid_argument`. All are checked before the ticket changes state, so it
stays prepared; at scale 2 the same ticket records after three
`invalid_argument` rejections. BGRA8 or 4 samples declared against a second
pass (differing only in an attachment-reference layout) return
`unsupported`, and that pass then renders correctly with a new ticket. A
second `record` returns `invalid_frame`. The extent is per `record`: the
conformance suite uses one renderer for 128x128 and 1280x720 cases.

## Per frame

```cpp
// 1. Host: collect() has retired the slot's last frame. Reset its fence
//    (prepare requires it unsignaled), reset and begin its command buffer.
wui::vulkan::FrameSlot slot{index, commands[index], fences[index]};
// 2. Uploads: barriers and vkCmdCopyBufferToImage per texture, recorded into
//    slot.commands outside any render pass.
auto prepared = renderer->prepare(slot, packet);
// 3. Host: vkCmdBeginRenderPass (clear if wanted).
// 4. Draw into the command buffer given to prepare.
renderer->record(ticket, {render_pass, 0, {width, height}});
// 5. Host: rebind its own state before drawing more, end the pass and the
//    buffer, vkQueueSubmit(queue, ..., slot.completion).
renderer->submitted(std::move(ticket));
// 6. After the fence signals and before it is reset:
renderer->collect();
```

- `Target` has no command buffer. `record` writes into the one given to `prepare`, so uploads and draws always share a command buffer.
- `prepare` returns `invalid_frame` if the fence is already signaled (tested with a freshly created signaled fence), `invalid_argument` for a slot index `>= max_frames` or a null command buffer or fence, `invalid_packet`, `capacity` (more than 256 textures) or `backend_failure`. The slot stays free on every error.
- A packet with no batches or indices records nothing. Its slot still completes normally.
- `record` binds its pipeline, vertex and index buffers, viewport, per-batch scissor, descriptor set and push constants, and restores nothing.
- `submitted` and `abandon` are `noexcept` and ignore tickets in the wrong state. Call `submitted` only after `vkQueueSubmit` with `slot.completion` succeeded.

If the host discards a command buffer without submitting it, it calls
`abandon(std::move(ticket))`; destroying an unsubmitted `PreparedFrame` does
the same. The slot is free and its buffers and images are destroyed at
once, so reset the command buffer first. The tests abandon a prepared-only
and a recorded frame, then render the same packet correctly.

## Frames in flight and busy

Each slot index is one ticket. `prepare` returns `busy` while the slot is
prepared, recorded or submitted, before touching the fence or command
buffer. Unlike D3D12, `prepare` does not poll the fence: a completed slot
stays `busy` until `collect` retires it. `collect` calls `vkGetFenceStatus`
on each submitted slot, never waits, and on `VK_SUCCESS` frees the slot and
its packet and uploads. A fence reset before `collect` saw it signaled
keeps its slot busy until it signals again (from the code; no test). The tests get `busy` for a
fourth frame; with an injected `VK_NOT_READY` fence status, `collect` keeps
all three revisions resident and `shutdown` returns `busy`.

## Memory

Every upload is its own `VkBuffer` and `VkDeviceMemory` (no suballocation):
a vertex buffer, a `uint32` index buffer and a staging buffer per texture.
The type must be host-visible; coherent is preferred, lowest index first.
The renderer maps, copies, flushes when the type is not coherent or
`force_explicit_flush` is set, and unmaps. Flush ranges are widened to
`nonCoherentAtomSize` boundaries and clamped to end exactly at the
allocation end (`atom_aligned_range`; it and type selection are unit tested
in `wui-vulkan-lifetime`). In `wui-vulkan` the forced mode flushes every
upload buffer, the default only noncoherent ones, and validation is clean
both ways. Readback is the host's; the conformance host invalidates an
atom-aligned range before each read.

## Textures and uploads

Uploads are per slot. For each texture in the packet, `prepare` creates a
staging buffer, an optimal-tiling `R8G8B8A8_UNORM` image, a view and a
descriptor set from the slot's pool, and records `UNDEFINED -> TRANSFER_DST
-> SHADER_READ_ONLY_OPTIMAL` barriers around the copy. The pool is reset at
the slot's next `prepare`. All of it is destroyed when `collect` retires the
slot or the frame is abandoned.

There is no cross-packet cache and no key by handle or revision. Every frame
uploads every texture in its packet again (a 1024x1024 atlas is 4 MiB), and
snapshots from different `TextureStore`s cannot alias. Tests: checker
revisions 1-3 get distinct images in three slots, and an old gallery atlas
renders pixel-identically after a glyph update. Blending is premultiplied
`ONE`/`ONE_MINUS_SRC_ALPHA`; straight-alpha textures are premultiplied in
the fragment shader.

## Scissor

Scissors come from the shared `backends/renderer/scissor.hpp` helper
(`physical_scissor`), as in every other renderer. The clip is scaled to
physical pixels, edges within 1/1024 px of a pixel boundary are snapped,
and the result is floored and ceiled outward, then clamped to the target.
Batches with zero indices, an empty clip or an empty scissor are skipped.
The tests check a clip ending at 95.75 x 87.75 (last pixel 95, row 87) and,
at scale 1.25, a clip top of 57.6 (71.99999809 physical) that starts at row
72 with no fringe row. `wui-core` unit-tests the helper.

## Shutdown and recreate

`shutdown` calls `collect`, returns `busy` while any slot is not free, and
otherwise closes the renderer (idempotent). Afterwards `prepare` and
`record` return `invalid_frame` and `collect` does nothing. The destructor
runs `shutdown` if needed, asserts in Debug when frames are pending, and
destroys objects on `Config::device`, so destroy the renderer first. There
is no `recreate` call: shut down, destroy, `create` again. An old gallery
packet renders pixel-identically after reconstruction on the same device;
the conformance suite also replaces the `VkDevice`.

## Errors and device_lost

`VK_ERROR_DEVICE_LOST` maps to `device_lost`; other failed `VkResult`s map
to `backend_failure`. A lost fence status in `prepare` or `collect`, or a
lost result from a resource call in `prepare`, is terminal: `prepare`,
`record` and `collect` return `device_lost`, `shutdown` returns it and
closes, and the host creates a new renderer. **Device loss is only
simulated in tests**: an injected fence-status function returns
`VK_ERROR_DEVICE_LOST` while the real device stays healthy, and a
replacement renderer on the same device then renders the packet. Physical
device loss is never provoked.

## Common conformance suite

`tests/conformance/vulkan_backend.cpp` (`wui_conformance_vulkan`, CTest
`wui-conformance-vulkan-hardware`) drives the public API as a host would on
the first discrete Vulkan 1.1 GPU with a graphics queue (else the first
other one), with three command buffers and fences and
`force_explicit_flush` on. It enables `VK_LAYER_KHRONOS_validation` with
synchronization validation through `VK_EXT_layer_settings`
(`validate_sync`), falling back to `VK_EXT_validation_features`, and counts
warnings and errors for the case, lifetime and recreate groups (`SKIP`
without the layer). Loader messages are printed but not counted; on the
test machine that is one notice about a duplicate OBS implicit layer. For
in-flight, the host does not collect: three frames are accepted, the fourth
returns `busy`. Recreate destroys the renderer and the device and creates
both again.

Result (`win-all-release` and `win-all-debug`, 2026-10-03): RTX 2080 Ti,
API 1.4.351, `nonCoherentAtomSize` 64, layer 1.4.309 from the local SDK,
0 counted messages in every group. Against the D3D11 WARP reference: 2
identical, 86 within tolerance, 0 failed, worst max channel diff 2.
`wui-conformance-compare` needs D3D11 in the same build, so it is not part
of the `win-vulkan` presets. Details:
`docs/validation/d-backend-conformance.md`.

## Not exercised by tests

- Noncoherent memory, and any GPU but the RTX 2080 Ti, whose upload and readback memory is coherent; `force_explicit_flush` exercises only the flush calls.
- Physical device loss, `device_lost` from resource creation, and `record` after loss.
- `capacity` (more than 256 textures), calls after `shutdown`, and the destructor assert.
- A nonzero subpass, secondary command buffers, and host draws after `record` in the same pass.
- Render passes incompatible beyond the declared format and sample count; the renderer cannot detect them.
- `maxMemoryAllocationCount`: one allocation per buffer and image, and the renderer does not check the limit.
