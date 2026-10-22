# A10 foundation validation report

The example is an application-owned Windows/D3D11 gallery. The **corrective**
Debug/Release × WARP/hardware runs below replace the original A10 validation
and CPU measurements. All four native scenarios and measurement runs exited 0;
this does **not** prove real device removal, a distinct-DPI monitor transition,
manual mouse/keyboard use, GPU time, or zero full-gallery allocations.

## Environment and provenance

- Windows 10.0.19045.6466; VS 2022 x64 developer environment, MSVC
  19.44.35226 (toolset 14.44.35207), SDK 10.0.26100.0; CMake 4.1.2,
  Ninja 1.12.1. [Toolchain stdout](a10/logs/environment.log).
- `third_party/lock.json` SHA-256:
  `e94617cdfd17eb79c817b2d8af4abd025a9b90d22100156356f96ccac7592912`.
  [Offline dependency verification](a10/logs/dependencies-verify.log) exited 0;
  ordinary configure did not fetch dependencies.
- WARP: Microsoft Basic Render Driver, vendor `1414`, device `008C`, LUID
  `00000000:0000FE39`; hardware: NVIDIA GeForce RTX 2080 Ti, vendor `10DE`,
  device `1E07`, LUID `00000000:0000E5D8`. Both negotiated feature level
  `11_0` and an available D3D11 debug layer in both build configurations.
  `CheckInterfaceSupport` did not return a driver version for either adapter
  (`HRESULT 0x887A0004`); version is unavailable, not inferred.

The command table uses repository-root paths and the x64 environment from
`call out\native-env.cmd`. Each linked log records the complete command,
working directory, combined stdout/stderr, exit code and elapsed time.
`out/validation/a/corrective/` retains the raw runs; accepted outputs are
copied under [`a10/`](a10/).

| Command / scope | Exit and evidence |
| --- | --- |
| `cmake --preset win-d3d11-debug -DWUI_BUILD_EXAMPLES=ON`; `cmake --build --preset win-d3d11-debug` | 0, 0 ([configure](a10/logs/configure-debug.log), [build](a10/logs/build-debug-narrow-fix.log)) |
| `cmake --preset win-d3d11-release -DWUI_BUILD_EXAMPLES=ON`; `cmake --build --preset win-d3d11-release` | 0, 0 ([configure](a10/logs/configure-release.log), [build](a10/logs/build-release-narrow-fix.log)) |
| `ctest --preset win-d3d11-debug -E wui-win32-clipboard --output-on-failure` | 0; 11/11 ([log](a10/logs/ctest-debug.log)) |
| `ctest --preset win-d3d11-release -E wui-win32-clipboard --output-on-failure` | 0; 11/11 ([log](a10/logs/ctest-release.log)) |
| Focused Debug/Release D3D11, default allocation-failure, semantic reuse, aligned-allocation contracts | All exit 0; D3D11 7/7, 292 assertions; [Debug D3D11](a10/logs/d3d11-focused.log), [Release D3D11](a10/logs/d3d11-focused-release.log), [Debug allocation](a10/logs/probe-allocation-contracts.log), [Release allocation](a10/logs/probe-release-allocation-contracts.log), [semantic reuse](a10/logs/probe-semantic-reuse.log) |
| `wui_widget_gallery.exe --driver hardware` (Release, supervised ordinary window) | 0 after programmatic `WM_CLOSE`; [observed HWND and bounds](a10/review/interactive-observation.txt), [unobscured screenshot](a10/review/interactive-hardware.png) |
| Invalid/duplicate/conflicting CLI, malformed UTF-16 | 2; [unknown](a10/logs/cli-unknown.log), [duplicate](a10/logs/cli-duplicate-driver.log), [conflict](a10/logs/cli-conflicting.log), [malformed](a10/logs/cli-malformed-utf16.log) |
| Throwaway copied executable without fonts | 1; [exact missing Inter path](a10/logs/missing-font.log) |
| Release WARP validation launched with an unrelated working directory | 0; [different-CWD log](a10/logs/different-cwd.log) proves executable-relative fonts |
| Throwaway core-only configure with examples OFF; examples ON without required native modules | 0; 1 with the exact `WUI_BUILD_EXAMPLES requires Windows, text, Win32 and D3D11` diagnostic ([core-only](a10/logs/core-only-configure.log), [invalid](a10/logs/invalid-example-configure.log)) |

The privileged clipboard executable was not run or changed.

## Native interaction and visual evidence

Each command below was run on the named driver, not inferred from a GPU
model. [Every accepted screenshot path and SHA-256](a10/screenshot-manifest.txt)
lists the **164 PNGs** (10 prescribed states at each of four simulated scales,
plus one 180px-wide capture for each configuration). Each driver directory's
`validation.txt` lists the semantic roles, labels, flags, model values,
physical dimensions, scenario outcomes and debug-layer checks.

| Build / driver | Native scenario command (repository root) | Exit | Artifacts |
| --- | --- | --- | --- |
| Debug WARP | `out\win-d3d11-debug\wui_widget_gallery.exe --driver warp --validate out\validation\a\corrective\debug-warp` | 0 ([log](a10/logs/validate-debug-warp-full.log)) | [PNG matrix](a10/review/debug-warp-contact.png), [validation](a10/debug-warp/validation.txt) |
| Debug hardware | `out\win-d3d11-debug\wui_widget_gallery.exe --driver hardware --validate out\validation\a\corrective\debug-hardware` | 0 ([log](a10/logs/validate-debug-hardware.log)) | [PNG matrix](a10/review/debug-hardware-contact.png), [validation](a10/debug-hardware/validation.txt) |
| Release WARP | `out\win-d3d11-release\wui_widget_gallery.exe --driver warp --validate out\validation\a\corrective\release-warp` | 0 ([log](a10/logs/validate-release-warp.log)) | [PNG matrix](a10/review/release-warp-contact.png), [validation](a10/release-warp/validation.txt) |
| Release hardware | `out\win-d3d11-release\wui_widget_gallery.exe --driver hardware --validate out\validation\a\corrective\release-hardware` | 0 ([log](a10/logs/validate-release-hardware.log)) | [PNG matrix](a10/review/release-hardware-contact.png), [validation](a10/release-hardware/validation.txt) |

Each matrix was reviewed across all four scales and ten states; representative
full-size normal, pressed, focus, disabled and narrow images were also opened.
Inter renders the dotted capital İ and text baselines in the sampled images;
focus outlines are visible, control and panel rounding remains consistent,
shadows do not obscure labels, and the simulated-scale targets align with
semantic hit centers used by the scripted input. The [180px image](a10/debug-warp/scale-100/narrow-180.png)
clips the long heading/body text, with no new wrapping; the checkbox remains
horizontal and within bounds. The real Inter `Notifications` label measures
83.5px at 1×, so forcing a stacked checkbox at 180px was an invalid initial
validator assertion. [Original failed run](a10/logs/validate-debug-warp-first.log)
and [real-font layout diagnostic](a10/review/narrow-diagnostic.log) are retained;
that assertion was corrected, not the widget breakpoint or image pixels.

Programmatic same-HWND mouse input checked checkbox activation on release,
button activation exactly once, held/repeat frames and outside release.
Programmatic Tab/Space/Enter checked focus and state, repeat-key suppression,
and disabled controls resisting both pointer and keyboard attempts. The host
checked resize during capture, focus loss to a second app-owned same-thread
HWND, ten minimized steps with zero new submissions/presents, restored state,
and controlled healthy-device recreation with byte-identical retained-packet
readback from cleared targets. No forced hardware removal was attempted.
All four scenario logs show zero debug-layer corruption/errors/warnings.

At startup `GetDpiForWindow` was 96. The simulated 1×/1.25×/1.5×/2× matrix
changes render/input scale and physical client size, **not OS DPI**. A
synthetic `WM_DPICHANGED` applies a suggested RECT and checks target/input
alignment, but is not a real DPI transition. Two monitors were enumerated;
both report effective 96×96 DPI, so no distinct-DPI cross-monitor move was
possible. The ordinary window was visibly opened and inspected, then closed
programmatically; manual mouse clicks, manual typing and a physical DPI move
are not claimed.

## Real application steady-state measurement

For each configuration/scale, four Context packet slots were prewarmed with
retained packets, then released; 120 real presented warmup frames preceded
300 accepted real presented sample frames. Fixed `dt=1/60`, enabled unchanged
model, outside pointer, no focus. A batch restarts on unexpected input,
resize, DPI, suspension, recreation or model mutation; all accepted runs
show `attempt=1`. C++ `new`/`new[]`/aligned/nothrow requests and UI-thread
`steady_clock` elapsed time cover only `Context::begin_frame` through the
actual `build_gallery` and `Frame::finish`. Adapter drain, renderer/collect,
actual swap-chain `Present` and reporting are outside that counted boundary.
Direct C allocation, other threads, driver heaps and GPU time were not
measured. No samples were fabricated by isolated-target readback.

| Build / driver | Scale | C++ calls sum / min / max (300) | Bytes / frame | UI CPU min / median / p95 / max (µs) | Dataset |
| --- | ---: | ---: | ---: | ---: | --- |
| Debug WARP | 1 | 27900 / 93 / 93 | 13744 | 261.9 / 266.8 / 299.9 / 426.9 | [CSV](a10/debug-warp/scale-100.csv) |
| Debug WARP | 1.25 | 27900 / 93 / 93 | 14208 | 262.4 / 265.8 / 276.8 / 323.7 | [CSV](a10/debug-warp/scale-125.csv) |
| Debug WARP | 1.5 | 27900 / 93 / 93 | 14208 | 262.4 / 265.6 / 299.5 / 362.5 | [CSV](a10/debug-warp/scale-150.csv) |
| Debug WARP | 2 | 27900 / 93 / 93 | 16312 | 280.0 / 283.6 / 314.6 / 394.9 | [CSV](a10/debug-warp/scale-200.csv) |
| Debug hardware | 1 | 27900 / 93 / 93 | 13744 | 258.7 / 262.3 / 293.0 / 474.8 | [CSV](a10/debug-hardware/scale-100.csv) |
| Debug hardware | 1.25 | 27900 / 93 / 93 | 14208 | 262.6 / 266.0 / 324.9 / 718.2 | [CSV](a10/debug-hardware/scale-125.csv) |
| Debug hardware | 1.5 | 27900 / 93 / 93 | 14208 | 261.9 / 266.35 / 320.6 / 715.9 | [CSV](a10/debug-hardware/scale-150.csv) |
| Debug hardware | 2 | 27900 / 93 / 93 | 16312 | 283.0 / 288.6 / 325.8 / 471.0 | [CSV](a10/debug-hardware/scale-200.csv) |
| Release WARP | 1 | 13200 / 44 / 44 | 12960 | 34.2 / 36.2 / 40.8 / 121.3 | [CSV](a10/release-warp/scale-100.csv) |
| Release WARP | 1.25 | 13200 / 44 / 44 | 13424 | 34.8 / 37.0 / 42.9 / 114.2 | [CSV](a10/release-warp/scale-125.csv) |
| Release WARP | 1.5 | 13200 / 44 / 44 | 13424 | 36.5 / 39.1 / 49.5 / 77.7 | [CSV](a10/release-warp/scale-150.csv) |
| Release WARP | 2 | 13200 / 44 / 44 | 15528 | 40.3 / 42.9 / 54.0 / 188.5 | [CSV](a10/release-warp/scale-200.csv) |
| Release hardware | 1 | 13200 / 44 / 44 | 12960 | 34.8 / 37.2 / 98.3 / 416.9 | [CSV](a10/release-hardware/scale-100.csv) |
| Release hardware | 1.25 | 13200 / 44 / 44 | 13424 | 34.9 / 38.3 / 102.1 / 553.2 | [CSV](a10/release-hardware/scale-125.csv) |
| Release hardware | 1.5 | 13200 / 44 / 44 | 13424 | 33.5 / 36.0 / 50.3 / 184.2 | [CSV](a10/release-hardware/scale-150.csv) |
| Release hardware | 2 | 13200 / 44 / 44 | 15528 | 36.6 / 42.4 / 105.8 / 493.8 | [CSV](a10/release-hardware/scale-200.csv) |

[WARP Debug](a10/debug-warp/measurement.txt),
[hardware Debug](a10/debug-hardware/measurement.txt),
[WARP Release](a10/release-warp/measurement.txt), and
[hardware Release](a10/release-hardware/measurement.txt) include byte sums,
min/max and per-scale operation metadata. Geometry per frame is stable within
each scale: 548 vertices/1428 indices at 1×, 556/1464 at 1.25× and 1.5×,
592/1626 at 2×; all have 9 batches, 2 textures and 5 semantics. Every CSV
has 300 finite indexed rows and the exact documented header.

The historical [pre-reuse Debug WARP baseline](a10/debug-warp/measurement-baseline.txt)
records 108 allocation calls per frame; the current Debug run records 93.
The earlier sampler used an isolated target/readback and labeled its build
`DebugOrRelease`; its CPU timings are **not comparable** to the corrected
real-present measurements. Release measured 44 calls per frame, not zero.
The focused [semantic-reuse probe](a10/logs/probe-semantic-reuse.log) records
zero allocations across 300 warmed `Canvas::semantic` assignments, which
does not establish full-gallery zero allocations.

## Open evidence limits

Distinct-DPI monitor motion, manual mouse/keyboard exercise, actual
hardware-removal recovery and GPU timing remain unverified. The measured
full-gallery zero-allocation goal is unmet (93 Debug and 44 Release C++
allocation requests per frame). Those limits are not reclassified as passing
because the WARP/hardware scenario matrix and core suites passed.
