# Spike 1: what an `Addr` dereference costs on D3D12

**Status**: Proposed (until the first timed run on the Windows GPU machine)

Spike 1 of [ADR 0014](../decisions/0014-overhead-measurement.md) decides what dereferencing an
[`Addr`](../spec/03-memory-and-buffers.md#addr) costs on D3D12, where it is a raw view's heap index
and a byte offset, so each base costs a descriptor fetch. Chapters
[03](../spec/03-memory-and-buffers.md), [05](../spec/05-shader-abi.md),
[05 part 2](../spec/05-shader-abi-part2.md#addresses) and
[05 part 3](../spec/05-shader-abi-part3.md) stay `Proposed` until it reports. This note is the
spike's pre-registration: the workloads, metrics, statistics and decision rule below are fixed
before any measurement runs, and changing them invalidates the round. The results section is filled
in when the spike reports; nothing above it changes afterwards.

The note binds from the first timed run on the Windows GPU machine; until then it changes only
through dated entries under Amendments. Hosted CI compiles the harness and runs its correctness
gate on WARP, the software adapter, which measures nothing and is never timed. Timing waits for
the Windows GPU machine.

## Question

Is `Addr` dereference on D3D12 cheap enough for hot paths?

## Candidates

| Arm | Base fetch | Role |
|---|---|---|
| F, root fields | `gRootBytes.Load<uint4>((k << 20) + e·16)`: the data buffer through the root SRV, no descriptor | floor, the native minimum |
| H1, one hop | `Addr b = gRootBytes.Load2(8k)`; `ByteAddressBuffer d = ResourceDescriptorHeap[b.x]`, wrapped in `NonUniformResourceIndex` when divergent; `d.Load<uint4>(b.y + e·16)` | judged |
| H2, two hops | `Addr a = gRootBytes.Load2(0)`; `b` = `ResourceDescriptorHeap[a.x]`'s `Load2(a.y + 8k)`; then as H1 | judged for escalation |
| H1m, the module | `Kernels.slang`: `Addr b = rojoRHI::root<Root>().bases[k]`, `rojoRHI::nonUniform(b)` when divergent; `rojoRHI::load<uint4>(b, e)` | report-only: is what ships what was measured |

Every arm shares one host, one kernel body and one set of resources; only the base fetch differs.
Every pipeline uses the D3D12 backend's global root signature, version 1.1, with the flags
`CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | SAMPLER_HEAP_DIRECTLY_INDEXED`: parameter 0 is a root SRV at
`t0`, space 0, flagged `DATA_VOLATILE`, and parameter 1 one 32-bit root constant at `b1`, space 0,
set to 0 and unused ([reserved bindings](../spec/05-shader-abi.md#reserved-bindings)). One
shader-visible CBV/SRV/UAV heap of 133 slots holds the backend's raw view pairs (`R32_TYPELESS`,
`RAW` flag): slot 0 a null SRV; 1 and 2 the output's SRV and UAV; 3 and 4 table A; `5 + 2k` and
`6 + 2k` data view `k`, for `k` in 0 to 63. A one-slot shader-visible sampler heap is bound with
it, as the backend binds both, because the sampler-heap-indexed flag requires one; no kernel reads
it. Every buffer is in a `DEFAULT` heap, filled once by a
copy before any dispatch, so memory type is not a variable. The output is `N × 16` bytes, and every
arm stores thread `t`'s sum at byte `t · 16` through `ResourceDescriptorHeap[2]`; H1m stores it with
`rojoRHI::store<uint4>(Addr(1, 0), t, sum)`.

The 64 distinct bases are 64 raw views over 1 MiB slices of one 64 MiB data buffer: view `k` covers
bytes `[k · 2^20, (k + 1) · 2^20)` (`FirstElement` `k · 2^18`, `NumElements` 2^18), and 32-bit word
`w` of the buffer holds `mix(w)`, the hash of
[`BenchLoads.slang`](../../Shaders/Bench/BenchLoads.slang). F's root SRV is the data buffer's GPU
virtual address, so F reaches the same bytes at byte offset `k << 20` that H reaches through view
`k`; a root SRV read past its resource is a GPU-based validation error, so the slices share one
buffer rather than 64. The root block of H1 and H1m holds 64 `Addr`s, entry `k` being `(5 + 2k, 0)`;
table A holds the same 64 `Addr`s, and H2's root block holds `Addr` A, `(3, 0)`. H1m's root block
and output match H1's, so H1m against H1 isolates the module's lowering.

## Workloads

- **Kernel body.** Groups of 64 threads, `[unroll]` on both loops. Thread `t` picks a slice `k`,
  then for `j` in 0 to 15 reads the `uint4` at element `e = (j << 12) | (t & 4095)` of that slice
  (16-byte elements, all inside its 1 MiB), adds it to a `uint4` sum that starts at zero, and stores
  the sum.
- **Base modes**, for every arm: *uniform*, `k = 0`; *wave-uniform*, `k = (t >> 6) & 63`, one base
  per 64 consecutive threads, that is one per thread group; *divergent*, `k = t & 63`, a base
  per thread. Wave-uniform and uniform kernels carry no `NonUniformResourceIndex`; divergent ones
  do. F runs in every mode, and each H arm is judged against F in the same mode, so the slice is a
  byte offset for F and a descriptor for H over the same bytes and the descriptor fetch is the only
  variable. Against a uniform-only F, wave-uniform and divergent H would read 64 MiB to F's 1 MiB,
  and the tax would include the footprint.
- **Wave lanes.** Wave-uniform's base is one per thread group of 64, and a D3D12 wave never spans
  thread groups, so the base is uniform in every wave whatever its lane count, 4 to 128. Every
  device runs every kernel, wave-uniform included, WARP among them with its 128-lane waves.
- **`N` sweep.** 64; 1,024; 16,384; 262,144 and 1,048,576 threads, `Dispatch(N / 64, 1, 1)`. At
  `N` of 64 wave-uniform reads what uniform reads, as expected.
- **Variants.** `fetch`, the kernel as above, is judged. `alu`, report-only, follows each load with
  16 rounds of `v = v * 1664525u + 1013904223u` before adding it, because spike 2 found that a
  kernel doing nothing but fetch inflates a tax. Five `N` and two variants make 10 configurations,
  each with 12 kernels: 4 arms in 3 modes.

## Metrics

1. **Correctness gate.** Every output byte equals the CPU reference, computed with wrapping 32-bit
   arithmetic as HLSL does, for every kernel, `N` and variant, one dispatch each, with the debug
   layer and GPU-based validation on. The info queue reports no message of severity corruption,
   error or warning, read through an `ID3D12InfoQueue1` callback or by polling `ID3D12InfoQueue`
   where that is absent, after the fence wait so that a message about the last dispatch still
   counts; no message ID is filtered. The process writes no line on standard error. The gate proves
   it can fail on every run: a last process at `N` 1,024, `fetch`, changes word 0 of elements 0 to
   63 and of element `64k` of every slice `k` after the reference is computed, and every kernel
   must report exactly the threads that read a changed word: 64 uniform, 79 wave-uniform and 64
   divergent, or the gate fails. Divergent kernels then mismatch in all 64 slices and wave-uniform
   ones in each slice their thread groups reach, so the check covers every view the kernels use,
   not one word of slice 0.
   A kernel failing the gate fails the run; no timed run starts until every kernel passes on the
   same adapter.
2. **GPU kernel time.** Timestamp queries written before and after each dispatch, converted with
   the direct queue's `GetTimestampFrequency`; a UAV barrier separates consecutive dispatches,
   outside the timed span.
3. **Shader instruction count**, report-only, to explain a tax. DXIL instructions per kernel from
   `dxc -dumpbin`: the lines of the entry function's body that are not blank, a label or a comment,
   with a histogram of the `@dx.op.*` names. It is machine-independent and also produced on CI.
   Where the GPU's vendor has an offline compiler, its ISA instruction counts are added.

## Statistics

- Release harness, with no debug layer for timed runs and the debug layer and GPU-based validation
  for the correctness runs.
- Each repetition is one fresh process for one configuration that runs all 12 kernels, in the
  order F, H1, H2, H1m, each uniform, wave-uniform and divergent, in even processes and the reverse
  order in odd ones. 20 processes per configuration.
- In each process, per kernel: 50 warm-up dispatches, then 200 measured dispatches, in one command
  list; the process median is kept. The reported value is the median of process medians.
- A paired delta is the difference of two kernels' process medians within one process, never
  across processes. A comparison's interval is the two-sided 95% percentile bootstrap of the median
  of the per-process paired deltas, with 10,000 resamples over processes, seed 20261020.
- Windows has no thermal state, so the run controls power and clocks itself. It requires the
  High-performance or Ultimate Performance power scheme (`powercfg /getactivescheme`) and does not
  call `SetStablePowerState`, which needs developer mode and holds clocks below shipping speed.
  Each process checks AC power (`GetSystemPowerStatus`) at its start and end; a process off AC
  power at either is discarded. Where the vendor has a command-line tool (for NVIDIA,
  `nvidia-smi --query-gpu=temperature.gpu,clocks.sm`), GPU temperature and core clock are read at
  each process's start and end. A process whose end temperature exceeds its start by more than
  10 °C, or whose end clock is below 95% of the median start clock of the round's kept processes
  and the current one (discarded attempts excluded), is discarded. A discarded process is rerun
  after 120 s of rest; after 4 attempts it is not run, and one process of the 20 not run makes its
  whole configuration not run. Without a vendor tool the run states that limit and rests 30 s
  between processes.
- A stopped round may resume in the same output directory with the harness unchanged; the stopped
  process reruns and its earlier attempts count. A round stopped before all its processes ran is
  not run for every configuration it left short until it is resumed to completion.
- Known limit: the start and end readings are taken just before the process launches and just
  after it exits, so the 95% clock test sees near-idle clocks rather than clocks under load.
- A timed process that writes any line on standard error or exits non-zero stops the round.
- Recorded per run: GPU, driver, Windows build, hardware-accelerated GPU scheduling, power source
  and scheme, the Agility SDK, WARP (unused when timing), DXC, Slang and MSVC versions, the
  repository commit, and the temperatures and clocks above.
- One vendor's GPU is enough to record the result; another vendor may add a later note.

## Decision rule

The rule is judged at `N` of 1,048,576, variant `fetch`, each H arm against F in the same base
mode. A slowdown is material when the 95% interval of the paired delta excludes zero and the
median paired delta exceeds 5% of F's median of process medians in that mode, the per-operation
budget of ADR 0014, applied to kernel time as spikes 2 and 3 did.

- **Cheap.** H1 uniform and H1 wave-uniform show no material slowdown; a material speedup counts as
  cheap and is reported. Chapters 03, 05, 05 part 2 and 05 part 3 become `Accepted`. The D3D12 cost
  cells of `Addr`, [`rojoRHI::load`](../spec/05-shader-abi-part2.md#rojorhiload) and
  [`rojoRHI::store`](../spec/05-shader-abi-part2.md#rojorhistore), and their rows in the
  [cost summary](../spec/A-cost-summary.md), become `cheap` with the measured figures, while
  divergent bases stay `costly` with theirs. The recommendation of
  [ADR 0011](../decisions/0011-shader-abi.md) widens from root fields and heap indices to
  wave-uniform `Addr` bases for per-draw hot data, with no interface change.
- **Costly.** H1 uniform or H1 wave-uniform shows a material slowdown. The cells and rows stay
  `costly` and take the measured figures, the recommendation stays root fields and heap indices for
  hot data, and the four chapters still become `Accepted`, because the reference names no other
  encoding and the cost is then measured rather than pending.
- **Incomplete.** If H1 uniform, H1 wave-uniform, F uniform or F wave-uniform has no result at the
  judged configuration, the round decides nothing, the chapters stay `Proposed`, and a fix goes in
  as a dated amendment before a new round.
- **Escalation.** If H2's median of process medians exceeds twice F's in the same mode, in any
  base mode it ran in, or H1 divergent's exceeds three times F divergent's, the result is recorded
  and escalated to the owner, who decides whether a superseding decision is needed. Escalation is
  judged at `N` 1,048,576, `fetch`, and computed even when the verdict is Incomplete. The spike
  does not change the encoding by itself.
- The result changes the API reference in the same change that records it. The `unverified`
  markers on the D3D12 lowerings of chapter 05 are a separate gate, the backend's compile-and-run
  test of [ADR 0018](../decisions/0018-root-srv-and-reserved-bindings.md), which this spike does
  not clear.

## Procedure

- The harness is standalone C++ with hand-written HLSL and contains no RojoRHI code. It lives under
  `Tools/Spikes/AddrDereference/` on the branch `spike/d3d12-addr-dereference` only and is
  throwaway: production work follows on a clean branch. `Harness.cpp` is the D3D12 host;
  `Kernels.hlsl` holds F, H1 and H2; `Kernels.slang` holds H1m and imports
  `Shaders/RojoRHI/RojoRHI.slang` unchanged; `run.py`, standard-library Python, fetches the
  toolchain, builds everything outside the xmake targets and runs the correctness gate, the timed
  processes and the analysis. `run.py selftest` checks the analysis, the decision rule and the
  instruction counter on synthetic data.
- `run.py` fetches the pinned toolchain and checks each download by the SHA-256 of the archive
  before extracting it; a wrong hash stops the build. The DXC binaries are copied beside
  `slangc.exe`, so Slang emits DXIL through the pinned DXC.

| Pin | Version | Source | Kept |
|---|---|---|---|
| Agility SDK, `Microsoft.Direct3D.D3D12` | 1.619.6 | NuGet | `build/native/include/`, `D3D12Core.dll`, `d3d12SDKLayers.dll` |
| WARP, `Microsoft.Direct3D.WARP` | 1.0.21 | NuGet | `d3d10warp.dll` |
| DXC | v1.9.2607 | the `microsoft/DirectXShaderCompiler` release | `dxc.exe`, `dxcompiler.dll`, `dxil.dll` |
| Slang | v2026.14.1 | the `shader-slang/slang` Windows x86-64 release archive | the archive; `slangc.exe` is also checked against the hash in `xmake/setup.lua` |

- The compilers run with these flags; each of the 12 kernels is built once per variant with
  `-D ARM -D MODE -D ALU`, 24 DXIL blobs. H1m is also compiled with `-target hlsl` for the record.

```
cl /nologo /std:c++20 /EHsc /O2 /DNDEBUG /W4 /WX /permissive- /utf-8 /DNOMINMAX
   /DWIN32_LEAN_AND_MEAN /DUNICODE /external:W0 /external:env:INCLUDE /external:I <agility include>
   d3d12.lib dxgi.lib dxguid.lib
dxc -T cs_6_6 -E spikeMain -HV 2021 -O3 -WX
slangc Kernels.slang -I Shaders/RojoRHI -D ROJORHI_TARGET_HLSL -target dxil -profile sm_6_6
   -entry spikeMain -stage compute -warnings-as-errors all
```

- **Device.** The harness loads the Agility runtime through its exports, `D3D12SDKVersion` 619 and
  `D3D12SDKPath` `.\D3D12\`, and fails unless `D3D12Core.dll` loaded from the `D3D12` directory
  beside it, `d3d12SDKLayers.dll` from that directory too when validation is on, and, on WARP,
  `d3d10warp.dll` from its own directory. The `hardware` adapter is the
  first that `EnumAdapterByGpuPreference` returns for high performance without the software flag;
  `warp` is `EnumWarpAdapter`. The device is created at feature level 12_0 and the harness exits
  non-zero unless the highest shader model is at least 6.6 and the resource binding tier is 3; it
  records the highest feature level. It uses legacy barriers: a UAV barrier between dispatches and
  implicit promotion for the initial copies.
- A workflow that exists only on the spike branch, `.github/workflows/spike-1.yml`, builds the
  harness on a hosted Windows runner and runs `run.py correctness --adapter warp` and
  `run.py selftest`; it uploads the DXIL disassembly, the instruction counts, the device limits and
  the correctness results. `run.py timing` refuses WARP, refuses to run without a passed
  correctness run on the same adapter in the same output directory, and refuses to run off AC
  power.
- **Side probe**, report-only: `Harness viewcap` creates a raw SRV of 2^27 + 1 elements over a
  buffer of 2^29 + 4 bytes under the debug layer and records whether the layer reports it, and the
  message ID. That bound caps one raw view, and so one `Addr` base, at 512 MiB; the probe decides
  nothing here, and `run.py correctness` runs it after the gate without letting it fail the gate.
- Raw timings, logs and the environment are kept outside the published tree, in
  `rojo-rhi-evidence/spike-1-d3d12-addr-dereference/<date>/` with a `SHA256SUMS` manifest
  ([recording GPU evidence](../guides/gpu-evidence.md)); this note keeps the environment, the
  summary tables and the decision. The harness as measured is tagged
  `spike-1-d3d12-addr-dereference-<date>`.
- The harness is compiled and gated on WARP only before the first timed run, so the Windows GPU
  machine may need fixes to it; they are made, and recorded under Amendments, before any timed run.

## Amendments

None.

## Results

Not yet measured.
