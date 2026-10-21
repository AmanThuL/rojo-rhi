# RojoRHI

RojoRHI is a dependency-free C++23 rendering hardware interface (RHI) library, thin, explicit and
honest over the shared conceptual core of Metal 4, D3D12 and Vulkan. Metal 4 through metal-cpp is
the only native backend implemented today; D3D12 is planned and Vulkan is a design oracle with no
code (ADR 0008). The v1 interface is what the code implements now and what Luminex uses; v2 is
specified in the decisions and the API reference below. The test-only Null backend implements it in
full, chapters 01 to 15, the C++ entries of 05 included (devices, resources, pipelines, command
recording, queues and fences, swapchains, upscalers, sparse tiles, diagnostics and counters), with
the validation tracker (ADR 0021): every valid-usage identifier of the API reference is checked and
has a death test. The Metal 4 backend of v2 is complete and is the default on macOS: it implements
the device, capabilities, formats, buffers, heaps, textures, views, samplers, queues, fences and
frames, shader libraries and pipelines with an archive cache, command lists (copies, fills,
clears, barriers, scopes, render passes, dynamic state, dispatches, direct, indirect and counted
draws, mesh tasks and MetalFX upscales), swapchains and presentation on a Metal layer, sparse tiles
and capture, with every scope, render pass and upscale timed on a device created with
`DeviceDesc::scopeTimings`. ADR 0001 owns the identity spelling every document here applies.
Luminex is its required consumer: it will mount the library as a submodule at `RojoRHI/` and drive
every change through a RojoRHI branch and pull request first ([commits](docs/conventions/commits.md)
owns that workflow). The repository is public and licensed under the Apache License, Version 2.0
([`LICENSE`](LICENSE)); it holds the library itself — public headers, shared implementation, the
Metal 4 backend, the optional ImGui adapter, tests and shaders — alongside its founding documents
(see [architecture](docs/architecture/overview.md) for directory responsibilities).

## Golden sources

- Conventions: [naming](docs/conventions/naming.md), [C++ style](docs/conventions/cpp-style.md),
  [commits](docs/conventions/commits.md), [documentation](docs/conventions/documentation.md),
  [testing](docs/conventions/testing.md)
- ADRs: [0001 identity and naming](docs/decisions/0001-identity-and-naming.md),
  [0007 address-first interface](docs/decisions/0007-address-first-interface.md),
  [0008 backends and the Vulkan oracle](docs/decisions/0008-backends-and-vulkan-oracle.md),
  [0009 conformance and valid usage](docs/decisions/0009-conformance-and-valid-usage.md),
  [0010 the hardware floor](docs/decisions/0010-hardware-floor.md),
  [0011 one shader ABI for every backend](docs/decisions/0011-shader-abi.md),
  [0012 language and dispatch](docs/decisions/0012-language-and-dispatch.md),
  [0013 RojoRHI creates no threads](docs/decisions/0013-threading.md),
  [0014 overhead is measured against native code](docs/decisions/0014-overhead-measurement.md),
  [0016 C++ format follows the No Graphics API style](docs/decisions/0016-cpp-format.md),
  [0017 v1 is frozen under Legacy/ until the consumer port](
  docs/decisions/0017-v1-frozen-under-legacy.md),
  [0018 the root block reaches D3D12 as a root SRV, and the module owns the bindings](
  docs/decisions/0018-root-srv-and-reserved-bindings.md),
  [0020 recording takes no locks in Release, except to claim a frame-arena page](
  docs/decisions/0020-recording-locks-scope-labels.md),
  [0021 the validation tracker records while recording and judges at submission](
  docs/decisions/0021-validation-tracker-replays-at-submission.md),
  [0022 versioning, with the version in the single public header](
  docs/decisions/0022-versioning-in-the-single-header.md),
  [0025 the per-operation CPU budget has a floor of 5 ns per RojoRHI call](
  docs/decisions/0025-cpu-budget-per-call-floor.md)
- API reference: [docs/spec/](docs/spec/00-conventions.md), entered through its conventions
  chapter
- Research: [docs/research/](docs/research/2026-10-11-rhi-redesign/01-nographicsapi.md), the
  evidence notes behind the decisions
- Guides: [extracting RojoRHI from Luminex](docs/guides/luminex-extraction.md),
  [recording GPU evidence](docs/guides/gpu-evidence.md)
- Architecture: [overview](docs/architecture/overview.md),
  [the Null backend](docs/architecture/null-backend.md),
  [command recording on it](docs/architecture/null-backend-recording.md),
  [its edges: presentation, upscaling, sparse, diagnostics](
  docs/architecture/null-backend-edges.md),
  [the validation tracker](docs/architecture/validation-tracker.md),
  [the tracker across queues](docs/architecture/validation-tracker-queues.md),
  [the Metal 4 backend](docs/architecture/metal4-backend.md),
  [command recording on it](docs/architecture/metal4-recording.md),
  [drawing on it: libraries, pipelines, passes, draws](docs/architecture/metal4-drawing.md),
  [its edges: presentation, sparse, capture](docs/architecture/metal4-backend-edges.md),
  [the benchmark harness](docs/architecture/benchmarks.md),
  [the D3D12 backend's wiring and floor probe](docs/architecture/d3d12-backend.md)

## Commands

Standalone:

```
xmake setup   # fetches pinned metal-cpp and Slang on macOS; Slang, Agility, WARP, DXC on Windows
xmake         # builds the frozen v1 library, RojoRHILegacy
xmake test RojoRHILegacyTests/unit   # runs the v1 CPU contract cases
xmake test RojoRHITests/contract     # runs the v2 contract cases on the default backend
xmake test RojoRHITests/death        # runs the v2 death tests (Debug only)
xmake test RojoRHITests/abi          # compiles the ABI shaders, checks shared layouts and, on macOS,
                                     # the packaged Metal shader library headers
xmake f -m debug -y                  # the default backend: metal4 on macOS, null elsewhere
xmake f -m debug -y --rojorhi_backend=null   # the Null backend; Tests/Core, Tests/Null need it
```

The Metal 4 backend builds on macOS only and needs the Metal toolchain for its kernels
(`xcrun metal --version`; `xcodebuild -downloadComponent MetalToolchain` installs it, as
CI does). After `xmake f -m debug -y` (the default on macOS; `--rojorhi_backend=metal4` says
it), the entries run under `MTL_DEBUG_LAYER=1` and need a real device:

```
xmake build RojoRHITests
xmake test RojoRHITests/contract      # [contract] and [metal4] cases, minus the death tests
xmake test RojoRHITests/death         # the death tests
xmake test RojoRHITests/abi           # the ABI cases
xmake test RojoRHITests/conformance   # the [conformance] cases (T1)
cd build/macosx/arm64/debug/rojorhi-test
MTL_DEBUG_LAYER=1 ./RojoRHITests "[contract]~[death],[metal4]~[death]"   # the contract entry by hand
```

`xmake f -m debug -y --rojorhi_backend=null` selects the Null backend again; `Tests/Core` and
`Tests/Null` build only with it, and hosted CI names its backend in every configure.

The v1 GPU conformance suite runs from its test directory, after `xmake build RojoRHILegacyTests`:

```
cd build/macosx/arm64/debug/rojorhi-legacy-test
MTL_DEBUG_LAYER=1 ./RojoRHILegacyTests "[gpu]~[.]"
```

Mounted inside a consumer's `RojoRHI/` checkout, the same commands take `-P .`, because xmake
resolves the project root to the outermost ancestor holding an `xmake.lua` — the consumer's root,
not this one:

```
xmake setup -P .
xmake -P .
xmake test RojoRHILegacyTests/unit -P .
xmake test RojoRHITests/contract -P .
xmake test RojoRHITests/death -P .
```

On a Metal 4 configure the same `-P .` applies to its entries, `conformance` included. The default
backend is `metal4` on macOS, so a consumer's hosted macOS CI, which has no GPU, passes
`--rojorhi_backend=null` to `xmake f`.

The D3D12 backend builds on Windows only: `xmake f -m debug -y --rojorhi_backend=d3d12` selects it,
with `--rojorhi_d3d12_adapter=hardware` (the default) or `warp` (the software rasterizer) and
`--rojorhi_d3d12_debug=mode` (the default: the debug layer in Debug, nothing in Release), `off`,
`layer` or `gbv` (the layer with GPU-based validation). Its own cases are tagged `[d3d12]`; the
`contract`, `death`, `abi` and `conformance` entries above run on it, on WARP in the hosted `d3d12`
CI job: a Debug configure with `warp` and `gbv`, a Release one with `mode` that runs `contract` and
`conformance` only, and a Debug one with `layer` for the upscale cases WARP cannot complete under
`gbv`; a per-case step re-runs a failed
entry one case per process. D3D12 is implemented, verified on WARP only, not on hardware
([the edges page](docs/architecture/d3d12-backend-edges.md#the-t1-run-on-warp)).

The benchmark harness (macOS only, [architecture](docs/architecture/benchmarks.md)):

```
xmake build RojoRHIBench              # builds the harness for the configured backend
xmake test RojoRHIBench/null          # runs the [null] and [host] cases, no GPU
xmake test RojoRHIBench/metal4        # runs the [metal4] cases, Metal 4 only
xmake test RojoRHIBench/native        # runs the [native] cases, local Metal 4 Apple Silicon only
python3 Tools/bench.py build          # builds Release and Debug, copies the binaries, lists configurations
python3 Tools/bench.py correctness    # runs the correctness gate, R against N, in both builds
python3 Tools/bench.py timing         # runs the timed processes, local hardware only (ADR 0014)
python3 Tools/bench.py analyze        # writes summary.json and analysis.txt from the results
```

`bench.py build` takes `--backend metal4` (the default) or `null`; on `metal4` the harness links,
`xmake test RojoRHIBench/metal4` passes and `correctness` (R against N, both builds, untimed)
passes; no CI job builds it there. `correctness --arms N` with the native arm alone smoke-runs it.
Timing follows ADR 0014: Release, validation off, on AC power, never on hosted runners. The first
timed round (2026-10-19) missed the CPU, GPU and allocation budgets and met the pipeline and
native-call ones ([results](docs/research/2026-10-18-metal4-overhead-results.md)); round 2
(2026-10-20), registered by an amendment to the note, measured the fixed backend, met the GPU,
pipeline and native-call budgets and missed CPU in 15 of 34 judged configurations and allocations
in 57 of 63 ([round 2 results](docs/research/2026-10-20-metal4-overhead-round2-results.md)).

`python3 Tools/check_lto_inline.py`, after `xmake build RojoRHILtoProbe` in a Release configure,
fails unless link-time optimization inlined `Device::capabilities` into its caller in the probe
(ADR 0012); the interface CI job runs it on macOS and Windows.

`python3 Tools/check_vu_coverage.py` fails when a valid-usage identifier of `docs/spec/` has
neither a death test nor an entry in `Tools/vu_pending.txt`, when an identifier has fewer death
tests than `ROJORHI_VU` sites in `Source/`, and on any `ROJORHI_VU` under `Backends/`; the policy CI
job runs it
([testing](docs/conventions/testing.md)).

v1 GPU conformance is checkpoint A, the cases tagged `[checkpoint-a]`
([ADR 0005](docs/decisions/0005-conformance-checkpoint.md)). The v2 interface's T1 conformance
cases of ADR 0009 are the `[conformance]` entry on Metal 4, one file for each category. Both need
`MTL_DEBUG_LAYER=1` and a real device on Metal 4 Apple Silicon, and there is no CPU-only substitute
for either.

The shader ABI module `Shaders/RojoRHI/RojoRHI.slang` and its hosted checks exist: the conformance
shaders under `Shaders/Tests/` compile to MSL, SPIR-V and HLSL on every host, and the build checks
the layout of the shared structs against Slang reflection. That is the hosted half of the T3 tier
(ADR 0009). DXIL compiles wherever a DXC loads, as on hosted Windows CI. On Metal the rule writes
the ABI stamp into each shader library (`.metal4lib`), and the Metal 4 backend's
`Device::loadShaderLibrary` reads that container, refuses another ABI version with
`ShaderAbiMismatch` and compiles pipelines from it; the `[conformance]` cases run the ABI
shaders on the GPU, the T3 tier's other half. The Null backend loads a text format (see [command
recording](docs/architecture/null-backend-recording.md)).

## Hard rules

- Creation returns `Result<T>`; misuse of the interface is fatal where it is checked. In the v2
  interface the check is `ROJORHI_VU`, with a valid-usage identifier, present in Debug builds only
  (Release leaves misuse unchecked) and tested by a death test or listed as pending; library
  invariants use `ROJORHI_INTERNAL_ASSERT`. v1 under `Legacy/` keeps its unconditional
  `ROJORHI_ASSERT` ([C++ style](docs/conventions/cpp-style.md), ADR 0009).
- Every GPU object is created with a label, defaulted to the `rojorhi.` prefix when the caller
  supplies none.
- Metal 4 is the only native backend implemented today, and the v2 one implements every entry:
  device, resources, queues, fences and frames, shader libraries, pipelines, command lists (copies,
  fills, clears, barriers, scopes, timed on request, render passes, dispatches, draws and MetalFX
  upscales), presentation, sparse resources and capture, refusing with `Unsupported` only what the
  reference lets it refuse. It is the default on macOS (`rojorhi_backend` is `null` elsewhere).
  D3D12 is planned and Vulkan is a design oracle with no code; no document claims either as a
  capability until it is real. The Null backend is test-only and implements every chapter (ADR
  0008).
- Every check lives in `Source/Core`; a backend entry calls the core's check first, then
  translates, and holds no `ROJORHI_VU`. A Metal 4 entry that calls an autoreleasing factory opens
  an autorelease pool, and so does every retire hook ([Metal 4 backend](
  docs/architecture/metal4-backend.md)).
- Rules for v2 work: the v2 interface is address-first and has no binding slots (ADR 0007); it is
  a C++23 subset with one backend per build, chosen by an xmake option, and no virtual interfaces
  (ADR 0012). The v1 code still has binding slots and virtual interfaces until it is ported to v2.
- v1 lives frozen under `Legacy/` until the consumer port deletes it (ADR 0017): its code moved
  unchanged, and a change there is a defect fix only, in a commit that touches nothing outside
  `Legacy/` except `xmake/targets.lua` and carries a `V1-fix:` trailer. The commit that deletes
  `Legacy/` is exempt. The project policy check enforces it.
- The module owns every shader binding and each shader compile defines exactly one
  `ROJORHI_TARGET_*` macro; a consumer shader declares no binding (ADR 0018). Changing a reserved
  binding or the value a built-in returns bumps `ROJORHI_SHADER_ABI_VERSION` (ADR 0024).
- Release recording takes no locks except when a list claims a new frame-arena page, for
  `uploadFrameData` or for a debug scope's label; a lookup that only feeds a Debug-tier check is
  compiled out of Release builds (ADR 0020).
- Public headers are dependency-free: no consumer header, no consumer namespace, and no backend,
  ImGui, windowing or third-party type (ADR 0007, ADR 0012).
- A public include is rooted exactly `rojoRHI/`, in that spelling, and is written in angle form
  (ADR 0001). The case-insensitive file system will not catch a misspelling, so the check does.
- An edit made through a consumer's mounted submodule is still a RojoRHI commit: it is reviewed
  and merged here, never folded into the consumer's history.
- No AI or tool attribution in any commit message or pull request.

## Update policy

Refresh this file whenever a command, architecture contract or hard rule changes.
