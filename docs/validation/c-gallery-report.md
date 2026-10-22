# C gallery validation report

Evidence for the C4 gallery milestone:
the complete renderer-neutral widget gallery replayed on a real D3D11 window,
measured under three stress workloads, and reviewed visually. Every number
below comes from a run recorded on 2026-10-03 on branch
`feat/widget-foundation`; nothing is predicted.

## Environment

| Item | Value |
| --- | --- |
| OS | Windows 10 Pro 10.0.19045 |
| Compiler | MSVC 19.44.35226 (`_MSC_FULL_VER` 194435226) |
| Release flags | `/DWIN32 /D_WINDOWS /EHsc /O2 /Ob2 /DNDEBUG` (recorded by the binary) |
| Hardware adapter | NVIDIA GeForce RTX 2080 Ti, vendor 0x10DE device 0x1E07, feature level 11_0 |
| WARP adapter | Microsoft Basic Render Driver, vendor 0x1414 device 0x008C, feature level 11_0 |
| UMD version | unavailable: `CheckInterfaceSupport(ID3D11Device)` returns 0x887A0004 on both adapters |
| D3D11 debug layer | available in every run; 0 errors, 0 warnings in every validation and measurement batch |
| OS DPI | per-monitor aware; scales below are simulated render/input scales, not OS DPI changes |

## Native D3D11 scenarios (`--validate`)

Commands (the directory names differ only by build and driver):

```bat
out\win-d3d11-debug\wui_widget_gallery.exe --driver warp --validate out\validation\c\red
out\win-d3d11-debug\wui_widget_gallery.exe --driver hardware --validate out\validation\c\debug-hardware
out\win-d3d11-release\wui_widget_gallery.exe --driver warp --validate out\validation\c\release-warp
out\win-d3d11-release\wui_widget_gallery.exe --driver hardware --validate out\validation\c\release-hardware
```

All four combinations exit 0. Each `validation.txt` has 42 `PASS` lines and ends
with `PASS programmatic validation; manual/visual review remains separate`.

| Scenario | Debug WARP | Debug HW | Release WARP | Release HW |
| --- | --- | --- | --- | --- |
| scale-100/125/150/200 (normal, mouse checkbox/button, keyboard, disabled) | PASS | PASS | PASS | PASS |
| scenario-01-tabs-hide-show | PASS | PASS | PASS | PASS |
| scenario-02-choice-values | PASS | PASS | PASS | PASS |
| scenario-03-dropdown-edges | PASS | PASS | PASS | PASS |
| scenario-04-slider-capture | PASS | PASS | PASS | PASS |
| scenario-05-keyboard | PASS | PASS | PASS | PASS |
| scenario-06-text-editing | PASS | PASS | PASS | PASS |
| scenario-07-resize-matrix | PASS | PASS | PASS | PASS |
| scenario-08-visual-states | PASS | PASS | PASS | PASS |
| scenario-09-glyph-atlas-recreation | PASS | PASS | PASS | PASS |
| scenario-10-packets-in-flight | PASS | PASS | PASS | PASS |
| resize-during-checkbox-press, narrow-checkbox-clipping, real-focus-loss-while-captured, minimize-no-submit-no-present, controlled-device-recreation, synthetic-WM_DPICHANGED-message-path, real-monitor-DPI-enumeration-and-moves | PASS | PASS | PASS | PASS |

Captures: `scale-*/` and `scenarios/` under each directory above (the Release
hardware run is the reference set). Contact sheets:
`out\validation\c\release-hardware\contact-states.png` (states, popups, text,
sections, atlas) and `contact-matrix.png` (4 scales x 800x600, 1280x720,
1920x1080).

Defects found and fixed while building this evidence (each with a
regression test):

- A system `WM_MOUSELEAVE` arriving during a captured slider drag became a
  pointer move to (-1,-1) and snapped the value to 0. The Win32 adapter now
  ignores leave while a press is captured (`wui-win32`, "mouse leave during a
  captured press does not move the pointer").
- 1920x1080 clients could not be created on smaller monitors; the gallery
  window now raises its maximum tracking size (`WM_GETMINMAXINFO`).
- `Frame::text_input` copied the model string every frame to detect changes.
  Release hid it for "İstanbul" (small-string buffer), but any value longer
  than 15 bytes allocated once per frame, and Debug allocated a 16-byte
  iterator proxy every frame. The editor now flags its own model write; the
  `--frame-reuse` probe includes a long text input and a numeric input and
  failed with 300 allocations before the fix.

Limits of the native run, by design: synthetic `WM_MOUSEMOVE` does not move
the real cursor, so the validator cancels `TME_LEAVE` tracking after each move;
no IME is driven natively and the interactive clipboard is never touched (IME
is covered by `wui-win32`, clipboard by the elevated `wui-win32-clipboard`
suite); duplicate choice values cannot be expressed by the fixed gallery and
remain covered by core choice tests.

## Measurements (`--measure`)

Commands, for `<d>` in `hardware` and `warp`:

```bat
out\win-d3d11-release\wui_widget_gallery.exe --driver <d> --measure out\measure\c\release-<d>
out\win-d3d11-release\wui_widget_gallery.exe --driver <d> --measure out\measure\c\release-<d> --workload controls100
out\win-d3d11-release\wui_widget_gallery.exe --driver <d> --measure out\measure\c\release-<d> --workload controls1000
out\win-d3d11-release\wui_widget_gallery.exe --driver <d> --measure out\measure\c\release-<d> --workload virtual10000
```

All eight runs exit 0. Every accepted batch is 4-slot prewarm + 120 warmup +
exactly 300 sampled frames = 420 successful Presents; per-frame rows are in
`out\measure\c\release-<d>\<batch>.csv`. Allocation counts are UI-thread C++
`new` requests between host `begin_frame` and `finish` (not `malloc`, other
threads or driver allocations). GPU time is D3D11 `TIMESTAMP_DISJOINT` plus two
`TIMESTAMP` queries around the UI renderer submission; 300/300 samples were
non-disjoint in every batch. Interrupted batches were discarded and rerun
(`DISCARD` lines in the reports; at most one per batch).

### Fixed-gallery zero-allocation gate

`PASS gallery zero-new 300/300 presented frames` for every batch on both
drivers, Release and Debug WARP (`out\measure\c\debug-warp`): `cpp_alloc_calls`
and `cpp_alloc_bytes` are 0 in all 300 warmed samples of each batch.

| Batch (Controls section) | Physical | Vertices / batches | CPU µs median / p95 (HW) | GPU µs median (HW) | CPU µs median / p95 (WARP) | GPU µs median (WARP) |
| --- | --- | --- | --- | --- | --- | --- |
| gallery-1280x720 @1 | 1280x720 | 2110 / 39 | 110.5 / 174.7 | 30.5 | 111.2 / 142.9 | 1284.8 |
| gallery-scale-100 | 800x600 | 1902 / 38 | 106.5 / 130.5 | 24.4 | 106.1 / 114.2 | 815.8 |
| gallery-scale-125 | 1000x750 | 1934 / 38 | 107.6 / 168.1 | 33.3 | 108.0 / 182.4 | 1169.3 |
| gallery-scale-150 | 1200x900 | 1958 / 38 | 108.1 / 138.7 | 45.1 | 109.2 / 172.0 | 2039.3 |
| gallery-scale-200 | 1600x1200 | 2178 / 38 | 117.8 / 220.8 | 73.7 | 122.1 / 163.4 | 4167.1 |

Retained Context memory: 5,600 bytes used and peak in every gallery batch
(ceiling 16,777,216). Process peak working set: 61.6–67.0 MB (hardware),
55.3–85.0 MB (WARP, which rasterizes in-process).

### Stress workloads (logical/physical 1280x720, compact theme, gap 0)

| Workload | Buttons emitted | Vertices / batches | C++ allocs | CPU µs median / p95 (HW) | GPU µs median (HW) | CPU µs median / p95 (WARP) | GPU µs median (WARP) | Retained peak |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| controls100 (10x10 labeled) | 100 | 7176 / 201 | 0 | 331.6 / 457.4 | 2028.6 | 330.0 / 455.0 | 1495.2 | 23,224 B |
| controls1000 (40x25 icons) | 1000 | 44008 / 2001 | 0 | 2369.3 / 5587.9 | 2927.6 | 2424.5 / 13710.2 | 11419.9 | 180,744 B |
| virtual10000 (visible_rows) | 26 of 10,000 | 1532 / 53 | 0 | 79.4 / 165.8 | 31.5 | 78.8 / 98.9 | 1216.0 | 4,792 B |

The stress workloads are not gated; they happen to report zero allocations as
well. Equivalent workloads were compared only with each other. Observations:

- `virtual10000` emits exactly `visible_rows(10000, 28, 0, 720)` = 26 rows
  (the 26th is partially clipped); 10,000 rows never become 10,000 widget calls.
  The core test also checks persistent row IDs across a scroll.
- `controls1000` produces two draw batches per icon button (rounded rect, then
  the icon texture), so batch count, not CPU build time, dominates GPU cost.
- On hardware, `controls100` GPU median (2.0 ms) is far above the gallery's
  (30 µs) despite fewer batches than `controls1000`; timestamps bracket the
  submission on the GPU timeline and include queue stalls, so per-workload GPU
  figures are an upper bound, not shader cost.

## Visual review

Self-review against the spec (Release hardware captures):

- Legibility, font baselines, focus rings, selected tabs/radios and the
  invalid red borders with messages render as intended at all four scales;
  no alpha fringes were visible at 1x–2x.
- The tooltip appears below "Restore defaults" inside the viewport.
- At the bottom edge the Quality menu flips above its trigger and covers the
  radio group; at the top edge it opens below; both stay inside the viewport.
- At 200x400 (`popup-narrow.png`) row labels such as "Quality men…" and
  "Balanc…" are clipped by their cells; text clipping, not ellipsis, is the
  current behavior. The same happens to "Quality menu (edge anchored)" at
  scale 2 in an 800x600 client (`matrix-scale-200-800x600.png`).
- In the resize matrix the Volume slider sits at 0: each cell presses the
  slider at its left edge and releases after the resize, by design.
- The text selection highlight is narrow for one-grapheme selections.
- B probes (D3D9/Vulkan at 256x256) still consume the same gallery source;
  their tab labels are clipped at that size, as noted in B.

User verdict on the rendered dark/violet visual direction:

> approved

Recorded verbatim on 2026-10-03 after the user reviewed
`contact-states.png` and `contact-matrix.png`. The observations above (label
clipping without ellipsis, popup covering the radios at the bottom edge,
narrow one-grapheme selection highlight) were presented with the captures and
accepted as-is.

## CTest presets

Built and run on HEAD `72a0568`. `win32-*` and `win-d3d11-*` runs exclude
`wui-win32-clipboard` (`-E wui-win32-clipboard`); that elevated suite was not
run here and stays a separate, user-approved step.

| Preset | Result |
| --- | --- |
| core-debug | 9/9 passed |
| core-text-debug | 10/10 passed |
| win32-debug | 10/10 passed |
| win32-release | 10/10 passed |
| win-d3d11-debug | 13/13 passed (includes `wui-gallery-cli`) |
| win-d3d11-release | 13/13 passed |
| win-d3d9-debug | 12/12 passed |
| win-d3d9-release | 12/12 passed |
| win-vulkan-debug | 12/12 passed |
| win-vulkan-release | 12/12 passed |

## Unmet objectives and D/E obligations

- None of the C objectives above is unmet.
- D: full cross-backend screenshot conformance for D3D9/Vulkan and new
  D3D10/D3D12 renderers; batching by texture to cut icon-heavy batch counts.
- E: installable targets, notices and release integration evidence.
