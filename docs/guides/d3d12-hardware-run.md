# Running RojoRHI on the first Windows GPU machine

**Status**: Accepted

The D3D12 backend is implemented, verified on WARP only, not on hardware. WARP is the software
rasterizer of hosted Windows CI: its runs are correctness evidence under the debug layer and
GPU-based validation, never T1 acceptance ([ADR
0009](../decisions/0009-conformance-and-valid-usage.md)) and never a measurement ([ADR
0014](../decisions/0014-overhead-measurement.md)). This guide is the ordered checklist for the first
machine with a Windows GPU, so that T1 acceptance (issue #21), the hardware markers of the API
reference (issue #22), spike 1 (issue #20) and the D3D12 overhead round (issue #21) need nothing
designed on the day. Each step gives its commands, what success looks like, what to record and
where, and what changes with the result.

Evidence goes where [recording GPU evidence](gpu-evidence.md) says:
`../rojo-rhi-evidence/<topic>/<date>/` with a `SHA256SUMS` file. A failure is a backend defect: file
one issue for each failing case and fix the backend; a case is never edited to pass. "The owner
decides" names the person who accepts the decision records of the repository.

## 1. The machine

- Windows 11 x64, build 22000 or later: the loader refuses an older build with `Unsupported` ([the
  D3D12 backend](../architecture/d3d12-backend.md#agility-loading)).
- A GPU that meets [the hardware floor](../decisions/0010-hardware-floor.md): feature level 12_2,
  shader model 6.6, resource binding tier 3, enhanced barriers, `DynamicDepthBiasSupported` and
  `TypedUAVLoadAdditionalFormats`. `createDevice` refuses a device below it and names the missing
  item (`Backends/D3D12/Source/D3D12Capabilities.cpp:63-102`, `checkFloor`).
- The current driver of the GPU's vendor.
- The MSVC the CI records ([the D3D12 build](../architecture/d3d12-build.md#toolchain)), xmake,
  Python 3 and git. `python3` must resolve: the CI job copies `python.exe` to `python3.exe` when the
  Windows installer provides only the former (`.github/workflows/ci.yml`, job `d3d12`).
- Optional: PIX (capture, section 4) and the vendor command-line tool `nvidia-smi`, which the
  thermal rule of sections 5 and 6 reads.
- On AC power, with the High performance or Ultimate Performance power scheme, and the state of
  hardware-accelerated GPU scheduling recorded.

`xmake setup` fetches the Agility SDK, WARP, DXC, Slang and the FidelityFX runtime, each checked by
hash (`xmake/setup.lua`). Record the machine first (`environment.json` has the macOS fields
`unknown` on Windows):

```
xmake setup
python3 Tools/gpu_evidence.py --topic d3d12-floor-hw --label machine -- powershell -NoProfile -Command "Get-CimInstance Win32_VideoController | Format-List Name,DriverVersion,DriverDate; (Get-CimInstance Win32_OperatingSystem).BuildNumber; powercfg /getactivescheme; reg query HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers /v HwSchMode"
python3 Tools/gpu_evidence.py --topic d3d12-floor-hw --label vendor-tool -- nvidia-smi --query-gpu=name,driver_version,temperature.gpu,clocks.sm --format=csv
```

`HwSchMode` is 2 when hardware-accelerated GPU scheduling is on and 1 when it is off. Success: the
log names one GPU, its driver and the scheduling state. `rojorhi_d3d12_adapter=hardware` takes the
first high-performance adapter that meets the floor; on a machine with two GPUs, disable the one not
under test in Device Manager first.

## 2. The floor on hardware

Configure for the hardware adapter with GPU-based validation, and build the tests:

```
xmake f -m debug -y --rojorhi_backend=d3d12 --rojorhi_d3d12_adapter=hardware --rojorhi_d3d12_debug=gbv
xmake build RojoRHITests
```

The floor probe creates its device with `EnumWarpAdapter` (`Tools/D3D12Probe/D3D12Probe.cpp:679`),
so it is not run here; the floor is read through the `[d3d12]` cases of `Tests/D3D12/`, from the
test directory:

```
cd build\windows\x64\debug\rojorhi-test
.\RojoRHITests.exe "[d3d12]"
```

Success: every `[d3d12]` case passes. The view heap run below prints the size the device reports
(the `OPTIONS19` maximum, capped at 2^21 descriptors) in the expansion of its first `CHECK`. Record:
that size; the resource heap tier and `DynamicDepthBiasSupported` (a created device has tier 2 or
higher and the capability, since the floor check refuses one without; read the exact tier from a
D3D12 capability viewer); `TiledResourcesTier` (`Backends/D3D12/Source/D3D12Capabilities.cpp:150`);
the adapter name and the driver. Keep it with:

```
python3 Tools/gpu_evidence.py --topic d3d12-floor-hw --label view-heap -- cmd /c "cd build\windows\x64\debug\rojorhi-test && RojoRHITests.exe -s ""the view heap is shader-visible and holds H descriptors"""
```

Topic: `d3d12-floor-hw/<date>/`. A device below the floor ends the run: the machine does not meet
ADR 0010.

## 3. T1 acceptance (ADR 0009, issue #21)

Debug with GPU-based validation (the configuration of section 2), every entry of `RojoRHITests`
(`xmake/targets.lua`), then Release, which enables no debug feature (`mode`), `contract` and
`conformance`. Each run goes through `Tools/gpu_evidence.py`, so the log of a failing run is kept:

```
xmake build RojoRHITests
python3 Tools/gpu_evidence.py --topic d3d12-t1 --label machine -- powershell -NoProfile -Command "Get-CimInstance Win32_VideoController | Format-List Name,DriverVersion,DriverDate"
python3 Tools/gpu_evidence.py --topic d3d12-t1 --label abi -- xmake test RojoRHITests/abi
python3 Tools/gpu_evidence.py --topic d3d12-t1 --label contract -- xmake test RojoRHITests/contract
python3 Tools/gpu_evidence.py --topic d3d12-t1 --label death -- xmake test RojoRHITests/death
python3 Tools/gpu_evidence.py --topic d3d12-t1 --label conformance -- xmake test RojoRHITests/conformance
xmake f -m release -y --rojorhi_backend=d3d12 --rojorhi_d3d12_adapter=hardware --rojorhi_d3d12_debug=mode
xmake build RojoRHITests
python3 Tools/gpu_evidence.py --topic d3d12-t1 --label contract-release -- xmake test RojoRHITests/contract
python3 Tools/gpu_evidence.py --topic d3d12-t1 --label conformance-release -- xmake test RojoRHITests/conformance
```

Topic: `d3d12-t1/<date>/`. Each case count must stay at or above the floors the CI job `d3d12`
checks in its step "Count the cases" (`.github/workflows/ci.yml`), which on 2026-10-21 were
`[conformance]` 173, `[d3d12]` 255 and `[contract]` 878;
`.\RojoRHITests.exe --list-tests "[conformance]"` prints a count.

No case was left out as PC-only in the full T1 run on WARP (the `d3d12` job of [run
39641225757](https://github.com/AmanThuL/rojo-rhi/actions/runs/39641225757),
[edges](../architecture/d3d12-backend-edges.md#the-t1-run-on-warp)). Two files are left out only on
WARP with GPU-based validation, `UpscalerTests.cpp` and `UpscaleConformanceTests.cpp`
(`xmake/targets.lua`, `contract_left_out`), because WARP never completes a FidelityFX dispatch under
it; on hardware they run in every configuration. If an upscale does not complete under GPU-based
validation there either, rerun those two files with `--rojorhi_d3d12_debug=layer` (the CI configure
`xmake f -m debug -y -o build/layer`) and report the hang. Cases that SKIP on WARP for a driver
limit (`maxDrawIndirectCount`, issue #109) are read in the log: a SKIP that is not a Debug-only
validation skip is a result to record.

Success: every case passes, unchanged, in Debug with GPU-based validation and in Release, with the
debug layer reporting no message of severity corruption, error or warning. Name the adapter and the
driver in each issue. If the reference itself is wrong for the driver, the owner decides. Nothing
changes until every case passes; then section 7.

## 4. The hardware markers (issue #22)

Each item below stays `unverified` in the API reference after the WARP evidence and waits for this
machine; items 7 and 17 also need a case first. The item numbers are those of [the WARP evidence
note](../research/2026-10-21-d3d12-warp-evidence.md), which also records the rules that WARP settled
and left out of this list. When an observation matches the expected value, the marker is deleted
from the reference line named, under the identifier stability rule. When it does not match, the
owner decides the correction.

- **Item 1, per-vendor `DynamicDepthBiasSupported`.** Observe: `createDevice` succeeds (section 2),
  and the cases that record a pipeline with `depthBias` and set the bias dynamically pass ("a depth
  bias is set while the bound pipeline has depth bias" in `Tests/Contract/RenderPassTests.cpp`,
  "depth bias moves a D32Float depth by the constant term" in
  `Tests/Conformance/DrawConformanceTests.cpp`). Expected: the capability is reported and honoured
  by the vendor's driver. Reference lines: `docs/spec/02-device.md` line 90 ("Per-vendor driver
  support for the last is unverified") and `docs/spec/06-pipelines-part2.md` lines 257-258
  ("per-vendor driver support for that cap is unverified") and 272 (the D3D12 `Cost` cell,
  "per-vendor support unverified").
- **Item 2, `OPTIONS19` shipping values, and the view heap (issue #22; pull request #86 set the view
  heap's floor).** Observe: the view heap size printed in section 2. Expected: at least 1,000,000
  plus the reserved slots, `2 + framesInFlight x 1,024`; WARP 1.0.21 reports 2,097,152. Record the
  value for the vendor and driver. Reference lines: the `maxViews`, `maxSamplers` row of
  `docs/spec/02-device.md` (the words "unverified shipping values") and the `maxViews` row of
  `docs/spec/15-formats-and-limits-part2.md`. A smaller maximum is refused with `Unsupported` naming
  `maxViews` ([descriptor heaps](../architecture/d3d12-backend.md#descriptor-heaps)); report it.
- **Item 3, `ALLOW_ALL_BUFFERS_AND_TEXTURES` and resource heap tier 2.** Observe: `createDevice`
  passes the floor check of resource heap tier 2 and the buffer and texture cases pass
  (`Tests/D3D12/D3D12BufferTests.cpp`, `Tests/D3D12/D3D12TextureTests.cpp`). Expected: pass.
  Reference line: the `Descriptor` row of `docs/spec/03-memory-and-buffers-part2.md` ("unverified
  against the floor").
- **Item 7, `NonUniformResourceIndex` on a device.** `Tools/check_dxil.py` settles the compiler's
  part on every host. Whether a divergent index reads the right descriptor is the driver's, and no
  case reads through an index that differs between the threads of a wave, so a `[conformance]` case
  that does is added first; any machine can add it. Observe: that case passes here under GPU-based
  validation. Expected: each thread reads its own descriptor. Reference line: the D3D12 `Lowering`
  cell of `rojoRHI::nonUniform`, `docs/spec/05-shader-abi-part2.md` line 45 (the words "on a device
  unverified").
- **Item 17, a barrier from `Undefined` waits for the writes of its `SyncBefore` stages.** The
  `[d3d12]` case "a barrier from Undefined discards and names no earlier access" shows only that the
  layer accepts the lowering; neither layer checks ordering, and the wait is the driver's. A case
  that fails when the barrier does not wait is added first; any machine can add it. Observe: that
  case passes here under GPU-based validation. Expected: the writes before the barrier are complete
  for the stages after it. Reference line: `TextureBarrier` in
  `docs/spec/09-synchronization-part2.md` line 149 ("taken to wait for those writes
  (*unverified*)").
- **Item 20, DLSS and XeSS identifiers.** The backend integrates FSR only; its `upscalers` lists FSR
  when the FidelityFX runtime is beside the executable. Nothing runs here for DLSS or XeSS: the
  identifiers in `docs/spec/12-upscaling.md` stay `unverified` until a backend integrates each SDK
  and checks them against its headers. The FSR identifiers are covered by the evidence note. Run the
  FSR upscale cases (`UpscalerTests.cpp`, `UpscaleConformanceTests.cpp`) on the hardware as part of
  section 3.
- **Item 21, limits and format support on hardware.** Observe: the `[d3d12]` cases "the limits are
  the D3D12 column of the reference", "every format the table reports as sampled is sampleable by
  D3D12" and "the format table never exceeds the core's ceilings" pass, and the
  `FormatConformanceTests.cpp` cases in section 3 pass. Reference lines: the introductions that mark
  values taken from vendor documentation, `docs/spec/15-formats-and-limits.md` lines 9-11 and
  `docs/spec/15-formats-and-limits-part2.md` lines 8-9; the guaranteed-format paragraph of the
  former (lines 113-118) says the table runs on WARP only, not on hardware.
- **Layer 0 of an array texture, read through a non-array fetch (issue #90).** Observe: the case "a
  single-layer view of layer 0 takes the non-array dimension and a later layer the array one"
  (`Tests/D3D12/D3D12TextureTests.cpp:392`) passes under GPU-based validation with no
  `SRV_RESOURCE_DIMENSION_MISMATCH` or `UAV_RESOURCE_DIMENSION_MISMATCH` message. Expected: none.
  The result goes in issue #90, which asks for a decision about layers above 0.
- **The pipeline cache (issue #94).** The cases under `Tests/D3D12/D3D12PipelineCacheTests.cpp` pass
  on hardware, including "a second device loaded from pipelineCacheData hits every pipeline the
  first created". The three paths WARP cannot produce have no case: a blob written on one adapter
  and loaded on another (`D3D12_ERROR_ADAPTER_NOT_FOUND`, `D3D12_ERROR_DRIVER_VERSION_MISMATCH`: one
  `Warning`, an empty library, every creation a miss); a runtime that refuses an empty library
  (`DXGI_ERROR_UNSUPPORTED`: one `Warning`, `library 0` in `pipelineCacheData`); a `StorePipeline`
  failure other than `E_INVALIDARG`. No case or tool covers them; the owner decides whether to check
  the first two by hand (a blob from a `warp` build loaded by a `hardware` build; count warnings and
  misses).
- **Capture.** With PIX installed, run `.\RojoRHITests.exe "[d3d12][capture]"` (the case "without
  PIX, beginCapture returns Unsupported and starts nothing" SKIPs there) and the contract case
  "beginCapture with a path either starts a capture or returns an error, and endCapture ends one"; a
  `.wpix` file must result. Without PIX, `beginCapture` returns the reference's error
  ([diagnostics](../spec/14-diagnostics.md)).
- **Presentation and sparse resources.** The cases of `PresentationConformanceTests.cpp` and
  `SparseConformanceTests.cpp` pass; record `TiledResourcesTier` from section 2.

Items 13 and 18, and the FSR markers A and B of the evidence note (the `Inputs` and `Frame values`
rows of `docs/spec/12-upscaling.md`, lines 173 and 174), wait for a `[d3d12]` or `[conformance]`
case, not for this machine: any machine can add one, and a WARP run then settles them.

Evidence: `d3d12-floor-hw/<date>/` for items 1, 2, 3 and 21, `d3d12-t1/<date>/` for the cases.

## 5. Spike 1: what an `Addr` dereference costs on D3D12 (issue #20)

The pre-registration is [the spike note](../research/2026-10-20-spike-1-d3d12-addr-dereference.md).
It binds from the first timed run; its Procedure, Statistics and Decision rule are not changed on
the day. The harness lives on the branch `spike/d3d12-addr-dereference` only. From a clean checkout:

```
git switch spike/d3d12-addr-dereference
python3 Tools/Spikes/AddrDereference/run.py build
python3 Tools/Spikes/AddrDereference/run.py correctness --adapter hardware
python3 Tools/Spikes/AddrDereference/run.py timing --adapter hardware
python3 Tools/Spikes/AddrDereference/run.py analyze
```

(`run.py` writes `build/spike-1`; `build` needs MSVC on `PATH`, so use a Developer PowerShell.)

The power, thermal and standard-error rules are the note's
[Statistics](../research/2026-10-20-spike-1-d3d12-addr-dereference.md#statistics); `run.py` enforces
them, and `timing` refuses WARP. A harness fix made on the machine is recorded under the note's
Amendments before any timed run.

Record: copy `build/spike-1/` to `../rojo-rhi-evidence/spike-1-d3d12-addr-dereference/<date>/`; its
`SHA256SUMS` is the harness's own. Tag the harness as measured:
`git tag spike-1-d3d12-addr-dereference-<date>`.

The result changes, in one change that records it, the note's Results and Status, chapters 03, 05,
05 part 2 and 05 part 3, and the D3D12 cost cells of `Addr`, `rojoRHI::load` and `rojoRHI::store`
and their rows in the [cost summary](../spec/A-cost-summary.md), as the note's [Decision
rule](../research/2026-10-20-spike-1-d3d12-addr-dereference.md#decision-rule) says. An Escalation
goes to the owner.

## 6. The D3D12 overhead round (issue #21)

The T2 round on D3D12 needs the Windows GPU machine (issue #21) and follows ADR 0014. Its
pre-registration is [the D3D12 overhead note](../research/2026-10-20-d3d12-overhead.md), Status
Proposed. Before any timed run it must be accepted and frozen: merge the change that sets its
Status to Accepted with the date, record any amendment in its Amendments section, and run from a
clean checkout of the commit that merges it. Do not time against an unfrozen note.

The harness is verified on WARP only; no hardware result exists yet. From a Developer PowerShell on
AC power with the High-performance plan and nothing else running:

```
python3 Tools/bench.py --topic t2-d3d12 build --backend d3d12 --adapter hardware
python3 Tools/bench.py --topic t2-d3d12 correctness
python3 Tools/bench.py --topic t2-d3d12 timing
python3 Tools/bench.py --topic t2-d3d12 analyze
```

`--root` and `--date` default to `rojo-rhi-evidence` beside the main checkout and today's date;
`correctness` and `timing` take `--only C1,G2` to narrow the workloads, which a full round does not
use. `timing` refuses a WARP build. Record `rojo-rhi-evidence/t2-d3d12/<date>/` with its
`SHA256SUMS` and add the topic to [recording GPU evidence](gpu-evidence.md). The commit timing seam
of the GPU span is not in the harness yet, so the round reports `gpuNs` as the benchmark
architecture page says ([the benchmark harness](../architecture/benchmarks.md)).

## 7. After the run

- **The wording.** The sentence "implemented, verified on WARP only, not on hardware" becomes
  "conformant on <GPU>, driver <version>" in the documents that carry it, and only after every case
  of section 3 passes. The change is a superseding decision record of [ADR
  0026](../decisions/0026-d3d12-implemented-before-hardware.md), not an edit of it, together with
  the README rule of `Tools/check_project_policy.py`, which admits D3D12 in public copy only in a
  sentence holding the phrase above and neither "support" nor "accept".
- **The API reference.** The markers section 4 settled are deleted under the identifier stability
  rule; the evidence note records the hardware run.
- **The default backend.** The Windows default stays `null` (`xmake/targets.lua`, option
  `rojorhi_backend`); the owner decides whether `d3d12` becomes the default once section 3 passes.
- **The issues.** #20 closes with section 5; #21 closes with section 3 and the judged round of section 6. #22 stays open until
  every D3D12 `unverified` marker is settled. #94 closes with its cross-adapter checks, and #90 gets
  its observation. Every open issue that waits for the machine, #20, #21, #22, #90 and #94, carries
  the comment "needs the Windows GPU PC; see docs/guides/d3d12-hardware-run.md".
- **The record.** Add the topics `d3d12-floor-hw`, `d3d12-t1`, `spike-1-d3d12-addr-dereference` and
  the T2 topic to the table of [recording GPU evidence](gpu-evidence.md).
