# The D3D12 Backend

**Status**: Accepted

This page describes the D3D12 backend as it stands: the build wiring, the pinned runtime, the
way the Agility SDK loads, the floor probe and the Windows CI job. Every entry of the backend is
defined and `createDevice` returns `Unsupported`, so the backend creates no device and is not a
capability ([ADR 0009](../decisions/0009-conformance-and-valid-usage.md),
[documentation](../conventions/documentation.md)). Everything measured on WARP below is
"implemented, verified on WARP only, not on hardware".

## Build options

- `rojorhi_backend=d3d12` selects the backend. It builds on Windows only; a configure on another
  platform fails naming the platform. The Windows default stays `null`, and the `interface` CI job
  asserts it ([ADR 0012](../decisions/0012-language-and-dispatch.md)).
- `rojorhi_d3d12_adapter` is `hardware` (the default: the first high-performance hardware adapter
  that meets the floor of [ADR 0010](../decisions/0010-hardware-floor.md)) or `warp` (the software
  rasterizer, for hosted CI, which has no GPU).
- `rojorhi_d3d12_debug` is `mode` (the default: the debug layer and DRED in Debug builds, nothing
  in Release; it defines `ROJORHI_D3D12_DEBUG_MODE`, which the backend reads once it creates
  devices), `off`, `layer` (the debug layer and DRED) or `gbv` (the debug layer, GPU-based
  validation and DRED). The value is `mode` and not `auto` because xmake reads `auto` as unset.

Both are build configuration, not environment variables: RojoRHI reads and sets none.

## Pins and the runtime layout

`xmake setup` fetches and keeps these. Each hash lives in `xmake/setup.lua`, is the SHA-256 of the
downloaded archive, and is checked before anything is extracted.

| Pin | Version | What setup keeps |
|---|---|---|
| Agility SDK (NuGet package) | 1.619.6 | the headers, `D3D12Core.dll`, `d3d12SDKLayers.dll` |
| WARP (NuGet package) | 1.0.21 | `d3d10warp.dll` |
| DXC | v1.9.2607 | `dxc.exe`, `dxcompiler.dll`, `dxil.dll`, copied beside `slangc` |

The rule `rojorhi.d3d12_runtime` copies the runtime next to an executable after each build:

```
<exe dir>\D3D12\D3D12Core.dll
<exe dir>\D3D12\d3d12SDKLayers.dll
<exe dir>\d3d10warp.dll
```

The FidelityFX loader and upscaler DLLs are copied beside the executable only when the
third-party directory holds them, so their absence is a supported state. A host adds the rule to
its own executable and sets the value `rojorhi.thirdparty` to the absolute third-party directory
that setup fills. The Agility headers come first on the include path, so they shadow the older
`d3d12.h` of the Windows SDK.

DXIL is mandatory on a d3d12 configure: the shader rule probes one known-good DXIL compile
through the pinned DXC, and a failure fails the build instead of skipping DXIL. On the other
backends the probe may skip.

## Agility loading

The probe loads the Agility SDK through the device factory: it takes `ID3D12SDKConfiguration1`
from `D3D12GetInterface(CLSID_D3D12SDKConfiguration)`, calls
`CreateDeviceFactory(619, <absolute exe dir>\D3D12\)`, creates the device from that factory, and
reads the debug and DRED configuration from the same factory with `GetConfigurationInterface`. It
passes an absolute path, and the call worked on the first attempt. The backend has no device
creation yet (`createDevice` returns `Unsupported`); it is specified to take this factory path,
under which a consumer executable defines no `D3D12SDKVersion` or `D3D12SDKPath` export and the
debug configuration reaches the runtime the device uses.

The classic exports (`D3D12SDKVersion` 619 and `D3D12SDKPath` `.\D3D12\`) were also shown to work
and are the documented fallback; the backend does not use them.

Evidence, from the `-factory` and `-exports` probe variants on the hosted runner
([run 39398881860](https://github.com/AmanThuL/rojo-rhi/actions/runs/39398881860), and
[run 39404593529](https://github.com/AmanThuL/rojo-rhi/actions/runs/39404593529) after the probe
was tightened; the exports path is corroborated by the
[spike run 39402446591](https://github.com/AmanThuL/rojo-rhi/actions/runs/39402446591)):

- Method: factory, `CreateDeviceFactory` succeeded first try; exports, `D3D12SDKVersion` 619.
- Modules: exactly one `D3D12Core.dll` loaded, `<exe dir>\D3D12\D3D12Core.dll`, version
  1.619.6.0; the in-box `D3D12Core.dll` never loaded.
- WARP: exactly one `d3d10warp.dll`, `<exe dir>\d3d10warp.dll`, version 1.0.21.0, app-local.
- Debug layer: `ID3D12DebugDevice` answers and `ID3D12InfoQueue1::RegisterMessageCallback`
  succeeds.
- GPU-based validation: the layer's startup message (id 1016) reads "GPU-Based Validation is
  enabled", and the probe reports `gbvActive`.
- DRED: breadcrumbs and page faults forced on, and `ID3D12DeviceRemovedExtendedData` answers.

## The floor probe

`RojoRHID3D12ProbeFactory` and `RojoRHID3D12ProbeExports` (under `Tools/D3D12Probe/`) each create
a device on the app-local WARP at feature level 12_2, record the floor items of ADR 0010, the
loaded modules and their versions, the debug layer messages, a large raw view and a hidden-window
presentation, and print one JSON object. A probe exits 0 only when the floor is met on the
app-local WARP. The `d3d12` CI job prints both objects, writes a step summary table, uploads them
as the artifact `d3d12-probe`, and fails when the factory probe does not exit 0; the exports
probe's exit code is printed and does not gate.

Results, all "WARP on hosted windows-latest" (Windows Server 2025, build 10.0.26100.33438; the
identical result came from both variants):

| Item | Result |
|---|---|
| Agility / WARP version | 1.619.6.0 / 1.0.21.0 |
| Maximum feature level, highest shader model | 12_2, 6.9 |
| Resource binding tier, resource heap tier | 3, 2 |
| Typed UAV additional formats, enhanced barriers, dynamic depth bias | all supported |
| Tiled resources tier, conservative rasterization tier | 4, 3 |
| `OPTIONS19` sampler, sampler with static, view heap maximum | 2,097,152 each |
| Largest shader-visible view heap created | 2,097,152 |
| Raw SRV of 2^27 and 2^27 + 1 elements | no debug-layer error for either |
| Relaxed format casting, GPU upload heap, copy-queue timestamp queries (reported, not floor items) | all supported |
| Presentation on a hidden window | window, queue and swapchain created; 2 frames, `Present` S_OK |

The silent 2^27 + 1 view means the layer does not catch an over-long raw view, so the core's own
valid-usage check must. These are WARP results: not hardware evidence and not T1 acceptance.

## Toolchain

Recorded from the log of
[run 39415964342](https://github.com/AmanThuL/rojo-rhi/actions/runs/39415964342) on
windows-latest:

- xmake v3.1.2 passes `-std:c++23preview` to MSVC for `c++23`.
- MSVC tools 14.51.36231, Visual Studio 18, `cl` 19.51.36260 (`_MSC_FULL_VER` 195136260,
  `_MSVC_LANG` 202302).
- `/std:c++23preview` and `/std:c++latest` are accepted. `/std:c++23` is not: `cl` warns D9002 and
  ignores it.

Nothing changed because of this; xmake already uses the accepted spelling
([ADR 0012](../decisions/0012-language-and-dispatch.md)).

## Tests and CI

The `d3d12` job on `windows-latest` runs, in order: setup; a Debug configure with
`--rojorhi_d3d12_adapter=warp --rojorhi_d3d12_debug=gbv`; the two probes and their gate; the
`RojoRHITests` build; the check that every shader under `Shaders/Tests` has DXIL (except
`LayoutProbe`); the check that the runtime sits beside the test executable; `RojoRHITests/abi`
and `RojoRHITests/contract`; the toolchain record; the compilation database and the layout check;
then a Release configure with `--rojorhi_d3d12_debug=mode`, the `RojoRHITests` build (warning-free
under `/W4 /WX`), `contract` and the build of the link-time-optimization probe.

`d3d12_pending` in `xmake/targets.lua` lists every contract file today; the entry cannot create
a device yet. Each later part removes the files that pass whole on WARP under the debug layer
and GPU-based validation, and the list ends empty. A name that matches no contract file fails the
configure. There is no `death` or `conformance` entry until the backend creates a device. The
Release `RojoRHITests` and `RojoRHILtoProbe` executables turn off warnings C4702 and C4722
because the stubs abort; `check_lto_inline.py` does not run on d3d12.

The layout check splits by platform. The macOS `format` job passes `--exclude Backends/D3D12
--exclude Tests/Support/D3D12 --exclude Tools/D3D12Probe`; the `d3d12` job passes the matching
`--only` selectors with its own compilation database and `clangd`. A selector that selects
nothing fails, and a directory is named in either list only once it exists.
