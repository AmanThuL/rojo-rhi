# Architecture Overview

**Status**: Accepted

This page describes RojoRHI's shape as the code stands today: the v1 interface imported from
Luminex, implemented by the Metal 4 backend and frozen under `Legacy/` (ADR 0017). The top-level
`Include/`, `Source/`, `Backends/`, `Tests/` and `Shaders/` are the permanent names, reserved for
the v2 interface, which is specified in the ADRs and the API reference under `docs/spec/`. The
test-only Null backend implements all of it and its validation tracker is complete; the Metal 4
backend implements the device, its resources, queues, fences and frames, shader libraries, pipelines
and their cache, and records command lists, render passes, dispatches, draws, scopes and MetalFX
upscales included, presents swapchains, maps sparse resources and captures ([its
page](metal4-backend.md), [command recording on it](metal4-recording.md) and
[drawing](metal4-drawing.md)), with every scope, render pass and upscale timed on a device created
with `DeviceDesc::scopeTimings`. The shader ABI module and its hosted checks exist.

```
rojo-rhi/
  LICENSE  AGENTS.md  CLAUDE.md  README.md  xmake.lua  root project settings only
  .github/workflows/                          CI: policy; legacy, metal4, format, interface
  xmake/targets.lua  setup.lua  shaders.lua   targets are includable by a host project
  xmake/slang.lua                             the Slang compile and layout-check rules
  xmake/kernels.lua                           the Metal 4 backend's kernel build rule
  Include/rojoRHI/                            v2 public headers, dependency-free
  Source/Base/  Source/Core/                  v2 private base; the backend-neutral core
  Backends/Null/Source/                       the Null backend (no GPU)
  Backends/Metal4/Source/                     the Metal 4 backend
    Backends/Metal4/Kernels/                    the Metal 4 backend's own MSL kernels
  Backends/Metal4/MetalCpp/                   the one metal-cpp unit, shared with Legacy
  Adapters/ImGui/                             the backend-neutral ImGui adapter: Include/, Source/, Shaders/
  Tests/Contract/                             backend-neutral contract cases
  Tests/Core/  Tests/Null/                    core internals; Null expectations (Null builds)
  Tests/Metal4/  Tests/Conformance/           Metal 4 expectations; T1 readback oracles
  Tests/Support/                              the death-test harness; per-backend helpers
  Shaders/RojoRHI/  Shaders/Tests/            the shader ABI module; its conformance shaders
  Shaders/Bench/                              the Slang sources of the benchmark workloads
  Benchmarks/                                 the RojoRHIBench harness: both arms, host code, checks
  Benchmarks/HostWindows/  Benchmarks/NativeD3D12/  Benchmarks/ChecksD3D12/
                                              the Windows host; the D3D12 arm and checks
  Legacy/Include/rojoRHI/                     v1 public headers, dependency-free
  Legacy/Source/  Legacy/Source/Base/         v1 shared implementation; private base
  Legacy/Backends/Metal4/Source/  Legacy/Backends/Metal4/ImGui/
  Legacy/Tests/  Legacy/Shaders/Tests/        v1 contract, validation and GPU conformance
  Tools/                                      checks, Patches/, ImGuiBufferProbe/
  Tools/bench.py  Tools/bench_analysis.py     the benchmark driver and its statistics
  Tools/shader_layout.py                      generates the layout checks from Slang reflection
  Tools/shader_library.py                     packages a metallib into a .metal4lib
  Tools/Rename/  Tools/Extract/               the Luminex rename table and the extraction scripts
  docs/conventions/  docs/decisions/  docs/architecture/  docs/guides/
  docs/spec/  docs/research/                  the v2 API reference; the evidence behind it
```

## Directory responsibilities

`xmake.lua` at the root declares project settings, requires and the language level once; it is never
included directly by a host project. `xmake/` holds `targets.lua` (the includable target
declarations; the v1 targets are macOS only and `RojoRHIBench` builds on macOS and, with
`d3d12`, on Windows), a minimal `setup.lua` (metal-cpp and Slang pins, fetched on macOS; Windows
fetches Slang, the Agility SDK, WARP and DXC, as hash-checked archives), and a test-only copy of the
shader-to-metallib rule, so RojoRHI configures, builds and tests standalone.

`Include/`, `Source/`, `Backends/`, `Tests/` and `Shaders/` are the permanent tree of the v2
interface. `RojoRHI` is a static library of `Source/**/*.cpp` and the selected backend's sources;
the `rojorhi_backend` configure option accepts `metal4`, which builds on macOS only and is the
default there, `d3d12`, which builds on Windows only and takes the options `rojorhi_d3d12_adapter`
(`hardware` or `warp`) and `rojorhi_d3d12_debug` (`mode`, `off`, `layer` or `gbv`), and `null`, the
GPU-free Null backend and the default elsewhere. `Source/Base/` is
the private base (the Debug-tier `ROJORHI_VU` check and the message sink) and `Source/Core/` the
backend-neutral core every backend embeds; `Backends/Null/Source/`, `Backends/Metal4/Source/` and
`Backends/D3D12/` implement the public entries over it, each one backend per build
([the D3D12 backend](d3d12-backend.md) describes the last). `Backends/Metal4/MetalCpp/` holds
`MetalCppImpl.cpp`, the one unit that defines metal-cpp's class and selector tables: the
`RojoRHIMetalCpp` target compiles it on macOS for any backend option, and the Metal 4 backend,
`RojoRHILegacy` and `RojoRHIBench` link it, so a host that links more than one has one definition of
each table. `Backends/Metal4/Kernels/` holds the MSL files of the backend's fill, clear and
counted-draw kernels, which `xmake/kernels.lua` compiles on a `metal4` build into a library embedded
in the binary; that build needs the Metal toolchain. [The Null backend](null-backend.md) describes
both layers and what is implemented, [command recording](null-backend-recording.md) the recording
chapters, and [the edges](null-backend-edges.md) presentation, upscaling, sparse resources,
diagnostics and counters, and [the validation tracker](validation-tracker.md) the checks that follow
a resource across commands, lists, submissions and queues. `Include/rojoRHI/RHI.h` is the whole v2
public interface, one header in sections by API-reference chapter.

`Tests/` builds as the `RojoRHITests` binary, whose file set depends on the backend. With `null`,
`contract` runs the T0 cases of `Tests/Contract/`, `Tests/Core/` and `Tests/Null/` except the death
tests, `death` runs the `[death]` cases and `abi` the shader ABI cases. With `metal4`, `contract`
runs `Tests/Contract/` and `Tests/Metal4/`, `death` and `abi` run as on Null, and `conformance` runs
the `[conformance]` cases of `Tests/Conformance/`, which observe GPU results; every Metal 4 entry
runs under `MTL_DEBUG_LAYER=1`. With `d3d12`, `contract` also runs the `[d3d12]` cases of
`Tests/D3D12/`, which may include the backend's own headers, and the other entries run as on
Metal 4. `Tests/Core/` and `Tests/Null/` build with Null only. `Tests/Support/` holds the
death-test harness and the helpers of each backend, `Tests/Support/D3D12/` those of D3D12.
[Testing](../conventions/testing.md) owns the tiers. v1 lives frozen beside them under `Legacy/` and
is deleted with the consumer port; ADR 0017 owns the layout, the freeze and the consumer's
dependency rename.

`Shaders/RojoRHI/RojoRHI.slang` is the module every v2 shader imports: it owns every binding and
lowers the entries of chapter 05 per target, selected by one `ROJORHI_TARGET_*` macro (ADR 0018).
`Shaders/Tests/` holds compile-only conformance shaders that call every entry in every stage it
allows, and a layout probe; `Shaders/Tests/Modules/` holds a shared test module with no entry
point, which `SharedImport.slang` imports. `xmake/slang.lua` declares two rules: `rojorhi.slang`
compiles each entry file to MSL, SPIR-V and HLSL and packages a Null library (`.nulllib`) on every
host; it also compiles to a metallib when the Metal toolchain is present (packaged with the ABI
stamp and each entry's stage and threadgroup size into a `.metal4lib` by `Tools/shader_library.py`)
and to DXIL and a `.d3d12lib` when a DXC is reachable. Every input must declare an entry point, or
the build fails; shared modules are reached through `slang.includedirs`, and `slang.embed` writes
the configured backend's libraries into a generated header (`Tools/embed_library.py`).
`rojorhi.shader_layout` runs `Tools/shader_layout.py`, which reads the probe's reflection on each
target and generates `static_assert`s compiled into `RojoRHITests`. The tests tagged `[abi]` run
both. The header comment of `xmake/slang.lua` states the rule's full consumer interface.

`Benchmarks/` builds the executable `RojoRHIBench` on macOS and, on a `d3d12` build, on Windows.
It runs a native arm (Metal 4 or D3D12) and a RojoRHI arm of the same workloads in one process and
takes the measurements of the overhead round. The D3D12 build is verified on WARP only, not on
hardware. `Shaders/Bench/` holds the workloads' Slang sources beside `Shaders/Tests/`, and the
hand-written MSL lives with the native arm. Its RojoRHI arm runs on the Null backend until the
Metal 4 backend implements the entries it calls. [The benchmark harness](benchmarks.md) describes
the arms, the command line, the Null mode and the driver `Tools/bench.py`.

`Legacy/Include/rojoRHI/` is the v1 public surface: dependency-free headers naming no backend,
windowing, ImGui or third-party type, rooted at the `rojoRHI/` include spelling ADR 0001 fixes.

`Legacy/Source/` holds the shared implementation behind those headers, including
`Legacy/Source/Base/`, the private base (assertion, alignment, JSON escaping and `std::format`
logging) that replaces the Core dependency the component had inside Luminex.

`Legacy/Backends/Metal4/Source/` is the Metal 4 backend, built through metal-cpp;
`Legacy/Backends/Metal4/ImGui/` is the optional ImGui adapter, with its maintained patch and buffer
probe under `Tools/`.

`Adapters/ImGui/` is the backend-neutral ImGui adapter, `RojoRHIImGui`, written against the public
interface alone, so it runs on the Null, Metal 4 and D3D12 builds. Its header includes `<imgui.h>`
and lives in its own include root, `Adapters/ImGui/Include`, so `Include/` stays free of third-party
types. The v1 adapter above stays until `Legacy/` goes. [The ImGui adapter](imgui-adapter.md)
describes its target, host loop, frame buffers and platform windows.

`Legacy/Tests/` and `Legacy/Shaders/Tests/` hold the contract, validation and GPU conformance
suite, including the checkpoint A cases ADR 0005 (superseded by ADR 0009) freezes by name, carried
in from Luminex's flat test tree without changing their assertions. They remain today's GPU
conformance set until the v2 tiers of ADR 0009 exist.

`Tools/` holds `check_vu_coverage.py`, which requires a death test or a pending entry for every
valid-usage identifier of the reference, `check_lto_inline.py` with its probe `LtoProbe/`, which
shows a Release build inlining a library entry into its caller, the public-header check that rejects
an include root not spelled exactly `rojoRHI/`, the maintained `Patches/`, and `ImGuiBufferProbe/`.
`Tools/D3D12Probe/` holds the Windows probe executables that report, as JSON, whether WARP meets the
hardware floor and how the D3D12 runtime loaded ([the wiring page](d3d12-backend.md)).
`Tools/Rename/` holds the substitution table that renamed the component inside Luminex before the
extraction and stays as the record of what that rename did; `Tools/Extract/` holds the scripts that
ran the extraction itself and the verifier that checked its result, both kept for a future re-run.

`.github/workflows/` runs the project policy check on Linux; the legacy, metal4, format and
interface jobs on hosted macOS; and the interface job also on Windows. The metal4 job builds the
Metal 4 backend in Debug and Release and counts its cases per tag. The `d3d12` job, on hosted
Windows, builds the D3D12 backend and runs its entries on WARP: implemented, verified on WARP only,
not on hardware ([the run](d3d12-backend-edges.md#the-t1-run-on-warp)). The `tracker-stress` job,
also on hosted Windows, runs the tracker's hidden `[stress]` cases on the Null backend under MSVC
AddressSanitizer. Hosted macOS has no Metal 4
device, so the GPU cases run on Apple Silicon before merge ([testing](../conventions/testing.md)).

`docs/conventions/`, `docs/decisions/`, `docs/architecture/` and `docs/guides/` hold this
repository's own conventions, ADRs, architecture pages and procedures, standing alone from
Luminex's. ADRs 0002 to 0006 are the decisions the v1 code was built under: ADRs 0003 and 0006 are
superseded by ADR 0007, ADRs 0002 and 0004 by ADR 0008, and ADR 0005 by ADR 0009. They stay as
provenance.

`docs/spec/` is the API reference for the v2 interface: a conventions chapter and one chapter per
area, each entry giving semantics, valid usage and a mapping to Metal 4, D3D12 and Vulkan. It
specifies the interface and does not describe code that exists. Metal 4 is the only native backend
verified on a device, and its v2 backend is complete; D3D12 is implemented, verified on WARP only,
not on hardware, so its column is a mapping the WARP run has exercised and no hardware has, and
Vulkan is a design oracle with no code, so its column is designed only (ADR 0008, ADR 0026).

`docs/research/` holds the dated evidence notes the decisions cite, such as
`docs/research/2026-10-11-rhi-redesign/`. Notes record what was found and are not binding;
the ADRs and the reference carry the decisions.

## Host inclusion

A consumer includes `xmake/targets.lua`, never this repository's root `xmake.lua`: xmake resolves
a project root to the outermost ancestor holding an `xmake.lua`, so the root file's project
settings, requires and language level are declared exactly once, by whichever project is
outermost.
