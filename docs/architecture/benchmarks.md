# The Benchmark Harness

**Status**: Accepted

`Benchmarks/` builds the executable `RojoRHIBench` on macOS and, with the d3d12 backend, on Windows.
It measures the overhead of RojoRHI against native Metal 4 written in metal-cpp, as [the
pre-registration](../research/2026-10-18-metal4-overhead.md) specifies, and on D3D12 against native
D3D12 as [its note](../research/2026-10-20-d3d12-overhead.md) specifies: that note is the
authority on the workloads, the metrics, the statistics and the decision rules, and this page
does not restate its tables. It
describes what the harness is, how to run it and what it still lacks before a timed round
([ADR 0014](../decisions/0014-overhead-measurement.md)).

## Layout

```
Benchmarks/Main.cpp      the command line
Benchmarks/Common/       configurations, registry, runner, results, dumps, scene, shared data
Benchmarks/Host/         clock, allocation counter, thermal state, environment, executable directory
Benchmarks/Native/       the native arm, Native* files; Kernels/*.metal is hand-written MSL
Benchmarks/Rojo/         the RojoRHI arm, Rojo* files
Benchmarks/Checks/       the Catch2 cases
Benchmarks/HostWindows/  the Host/ interfaces on Windows (d3d12 builds only)
Benchmarks/NativeD3D12/  the native arm on D3D12; Kernels/*.hlsl is hand-written HLSL (d3d12
                         builds only)
Benchmarks/ChecksD3D12/  the Catch2 cases of a d3d12 build: [host], [native] and [d3d12]
Shaders/Bench/           the workloads' Slang sources, prefixed Bench
Tools/bench.py           the driver; Tools/bench_analysis.py holds the statistics and the rules
```

Everything is in namespace `rojoRHI::bench`. One directory per arm keeps the native and RojoRHI
files of a workload side by side by name (`NativeC3.cpp`, `RojoC3.cpp`). The Slang sources share the
tree of `Shaders/Tests/`, so `Tools/check_shader_imports.py` keeps one basename namespace.

There is one executable because the note requires one process to run both arms on one device. It
links `RojoRHI` of the configured backend and defines `ROJORHI_BENCH_BACKEND_NULL` or
`ROJORHI_BENCH_BACKEND_METAL4`. It compiles no metal-cpp unit of its own: it links
`RojoRHIMetalCpp`, the one a `metal4` `RojoRHI` and `RojoRHILegacy` link, on every backend, so the
link holds one copy of metal-cpp's tables. The target is built with link-time optimization in
Release, and `xmake/slang.lua` and the `rojorhi.metal` rule of `xmake/metal.lua` write each target's
generated headers and metallibs under that target's own directory.

## Arms

A workload arm is a class built from its `A::Context` (`NativeContext` or `RojoContext`) and a
`Config` that creates everything it needs, records, submits and waits one frame in `frame(index)`,
and returns its outputs from `readback()`. One template, `runArm<A>`, drives the warm-up and the
measured frames and takes every measurement, so both arms are measured by the same code and no
virtual call sits in a timed span.
Each workload family registers its configurations through one function: `registerCpuWorkloads`,
`registerCountedWorkloads`, `registerKernelWorkloads` and `registerFrameWorkloads`.

The native arm (N) is the registered minimum of Metal 4 calls for the workload. On a d3d12 build N
is the registered minimum of D3D12 calls instead, in `Benchmarks/NativeD3D12/`: it creates its own
device on the adapter R uses, loads the Agility runtime through its own device factory, keeps three
frame slots and its own fence, and uses the same DXIL as R for the Slang workloads and hand-written
HLSL for G1 and G2. The composed frame's N arm is not built on d3d12 yet: `runNativeFrame` returns
a failed result that says so, `frame` stays registered with R and N, and the correctness gate of a
d3d12 build selects every other workload. As a renderer with
three frames in flight, `NativeContext::endFrame` signals a shared frame event on the graphics queue
after the frame's commits, and `beginFrame` waits on the CPU for the value of the slot's last frame
before it resets the slot: one queue call and one host call a frame, as R's `endFrame` and
`beginFrame` make, except that N waits at a slot's first use too, for 0, so its frames count alike.
The RojoRHI arm (R) includes only `<rojoRHI/RHI.h>`, and records through `bench::List` and
`bench::QueueRef`: aliases of `CommandList` and `Queue` on a `metal4` build, tracing wrappers on a
Null build. G2 has a third arm, F, spike 2's fixed-binding variant; it runs after R and N in every
process.

## Configurations and the command line

`RojoRHIBench list` prints the configurations, one JSON object per line:
`{"id", "workload", "params", "arms"}`. The ids are `C<n>-k<K>`, `C10m1-k<K>`, `G1-uniform`,
`G1-divergent`, `G2-uniform`, `G2-divergent` and `frame`. The driver reads the list and never
hard-codes the sweep.

```
RojoRHIBench list
RojoRHIBench environment OUT.json
RojoRHIBench run ID --mode correctness|timing --order RN|NR --arms R,N[,F] --out OUT.json
                    [--dump DIR] [--validation]
RojoRHIBench check [Catch2 arguments]
```

A command exits 0 when it ran, even if the arms' outputs differ, and non-zero on a harness failure
with the reason on standard error. `--order RN` runs R first and F always runs last; `--arms` may
omit R or N. `--validation` sets `DeviceDesc::validation` and needs a Debug build. A timing run has
50 warm-up and 200 measured frames; a correctness run has 2 and 3, and `--dump` writes the last
frame's outputs as `DIR/<id>.<arm>.<output>.bin`. `environment` writes the device, its families,
whether it has Metal 4, the timestamp frequency, the OS and the thermal state.

`run` writes one JSON object, `schema` 1: the id, workload and parameters, the mode, the build
(`release` or `debug`), the backend, whether validation was on, the order, the device, the OS, the
thermal state at the start and the end, one object per arm and `outputsEqual` (R against N, null
when either is absent; G2 adds `outputsEqualF`). An arm object holds `status` and `error`, then
per-frame arrays of `cpuNs`, `gpuNs`, `allocations`, `nativeCalls`, `nativeBarriers` and, for R,
`cpuAllocations`, with `null` where a value does not exist, the `counters` after warm-up and at the
end and `frameCounters`, the whole `Counters` delta of the first measured frame, both for R only,
and `outputs` with a SHA-256 for each.

## What is measured, and where

Each measurement has one definition for both arms.

- **CPU span.** `mach_absolute_time` (on d3d12, `QueryPerformanceCounter`). R: `acquireCommandList`
  to `end`; N: `beginCommandBuffer` to `endCommandBuffer` (on d3d12, from the list's `Reset` to its
  `Close`). C9 records its `K` lists before the span, which times the `K` submit calls
  alone. The composed frame has no CPU span, so its `cpuNs` is null.
- **GPU span.** Every commit of both arms carries commit feedback, in every run. A frame's span is
  the earliest `GPUStartTime` to the latest `GPUEndTime` over its commits. A `FeedbackCollector`
  with preallocated slots receives them, and a frame is complete when every commit has reported.
  Each arm's `endFrame` waits for that feedback, so one frame runs at a time and each arm's wait on
  its frame event (N's in `NativeContext`, R's in the backend) returns at once; it still guards the
  slot's reuse, as in a renderer. The harness adds no other fence or event the note does not
  register to either arm; a queue's progress signal, which no registered workload makes, would be
  R's measured overhead. Both arms attach the same handler, a block that opens an autorelease pool,
  so their feedback allocates alike. On Null a submission completes inside the call and no GPU time
  exists.
- **Native calls on d3d12.** N counts a plain increment beside each call on an `ID3D12Device`,
  `ID3D12CommandQueue`, `ID3D12GraphicsCommandList`, `ID3D12Fence`, `ID3D12Resource` (`Map` and
  `Unmap`), query heap or descriptor heap; R's definition is unchanged. The GPU span is not measured
  on d3d12 yet (below), so no timestamp query is counted. The registered column is in
  [the D3D12 note](../research/2026-10-20-d3d12-overhead.md).
- **GPU span on d3d12.** None yet: the GPU span of a submission needs the backend's submission
  timing seam, which arrives with the composed frame's N arm, so a d3d12 run reports `gpuNs` as
  null.
- **Allocations on d3d12.** The harness replaces the global `operator new` and `operator delete`
  in every form and counts the C++ allocations of both arms; allocations made inside the D3D12
  runtime are not seen.
- **Native calls.** R: the per-frame delta of `nativeRecordCalls + nativeSubmitCalls +
  nativeHostCalls`, with `nativeBarriers` reported beside it and not added. N: a plain increment
  beside each call on a Metal device, queue, command buffer, encoder, argument table, residency set,
  event, heap or counter heap; a property set on a descriptor or on `MTL4::CommitOptions` is not
  counted. N's per-operation count must equal the note's N column for C1 to C9, or the gate fails.
- **Allocations.** `AllocationCounter` replaces `malloc`, `calloc`, `valloc`, `realloc`, `memalign`
  and `batch_malloc` of malloc zone 0, typed entries included, with counting forwarders (one relaxed
  atomic, all threads). `main` installs it first and checks at install that `malloc`, `calloc`,
  `realloc`, `posix_memalign` and `new` each count once, and refuses to run otherwise. R also
  reports `cpuAllocations`, RojoRHI's own `Counters::cpuAllocations` over the whole `frame()` call.
- **Pipelines.** R: the pipeline fields of `Counters`; N reports no pipeline figure, and its
  creations only add to its host calls.
- **Thermal state.** Read at the start and the end of every run; the driver discards a process that
  did not stay `nominal`.

## Null mode and the checks

A Null build runs every R workload on hosted CI: nothing is timed and no output is meaningful, but
the call sequence and the counts are. `RojoRHIBench check` is a Catch2 session over
`Benchmarks/Checks/`, selected by tag:

| Tag | Where it runs | What it proves |
|---|---|---|
| `[host]` | everywhere | the clock, the JSON writer, the allocation counter, the runner, the comparison of outputs |
| `[null]` | Null builds | every R workload's `Counters` delta at the note's K pair, the per-operation delta, the traced call sequence at K 1 and 2; in Debug with validation on, so the tracker accepts every barrier |
| `[native]` | local Apple Silicon | N's outputs against CPU oracles for C1 to C10, G1 and G2, its frame event's two calls a frame, and the composed frame's determinism across frames |
| `[metal4]` | `metal4` builds | R's per-operation count is at most N's plus `c`, and N's equals the note's column, for C1 to C9; R's steady frame allocates no more than N's in C4 to C7, and less per C8 pass; R's C1 to C3 and C10 frames, untimed, make four native calls and at least three allocations fewer than timed; R's C10m1 costs per draw N's native calls and allocations |

`xmake test RojoRHIBench/null` runs `[null],[host]`, `RojoRHIBench/native` runs `[native]` under
`MTL_DEBUG_LAYER=1`, and `RojoRHIBench/metal4` exists only with that backend. The `[metal4]` case is
compiled in every build. The interface CI job runs `null` in Debug and Release on macOS and requires
a non-zero `[native]` inventory, since hosted runners have no Metal 4 device.

## The commit timing seam

The Metal 4 backend implements `Backends/Metal4/Source/CommitTiming.h`, a private, test-only header
that no public header names. `rojoRHI::metal4::setCommitTiming(queue, callback, user)` makes every
later commit of that queue carry `MTL4::CommitOptions` feedback that calls `callback` from Metal's
completion thread, so RojoRHI starts no thread (ADR 0013), with a `CommitTime` (queue, a per-queue
serial counted from 1, start and end in seconds, and a failure flag); a null callback detaches,
which does not cancel the reports of commits made before the detach, so the caller keeps `user`
alive until that work completes. Each commit's options are released by their own feedback handler,
after the report, as N releases its options after its frame wait, so neither arm's CPU span holds
that release. Those calls are not counted in the device's counters.
`Queue::submit` makes exactly one commit per submission that names lists, and the backend commits
nothing else on a caller's queue except the empty commit that `destroyDevice` drains with, which is
never timed; the stager's internal queue is unreachable from the seam. The harness calls it from one
file, `Benchmarks/Rojo/RojoGpuTiming.cpp`, under `ROJORHI_BENCH_BACKEND_METAL4`, and the `[metal4]`
tests call it too; on Null the R arm reports no GPU time.

## The driver

`Tools/bench.py` runs the round and `Tools/bench_analysis.py` holds the statistics and the rules as
pure functions. Its commands take `--root`, `--topic` (default `t2`) and `--date`; the output is
`<root>/<topic>/<date>/`, the root defaulting to `rojo-rhi-evidence` beside the main checkout, also
when the driver runs from a git worktree ([recording GPU evidence](../guides/gpu-evidence.md)).

```
python3 Tools/bench.py build [--backend metal4|null|d3d12] [--adapter hardware|warp]
                                                       builds Release and Debug, copies the binaries
                                                        and Shaders/ into bin/ (on d3d12 the whole
                                                        output directory, with the D3D12
                                                        runtime), writes configurations.json
                                                        and bench-environment.json,
                                                        returns xmake to Null Debug
python3 Tools/bench.py correctness [--arms R,N] [--only C1,G2]
python3 Tools/bench.py timing [--only C1,G2]
python3 Tools/bench.py analyze                         writes summary.json and analysis.txt
```

- **Correctness** runs each configuration twice, in Release without validation and in Debug with
  validation and `MTL_DEBUG_LAYER=1`, compares R's dumped bytes with N's, and fails on any standard
  error line other than the validation layer's banner. A configuration passes only when R and N were
  both compared; a run of one arm is a smoke. N's per-operation count must equal the registered
  column. A Null build requires `--arms N`. A workload with a failing configuration is not timed.
- **Timing** refuses a Null build and battery power, removes `MTL_*`, `METAL_*` and `Malloc*`
  variables from the children's environment, skips results that exist (resume), renames a process
  whose thermal state was not nominal to `.discarded<k>.json`, rests 120 s before a retry, and marks
  the configuration not run after the fourth attempt. A timed process that prints any line on
  standard error stops the round with its result renamed `.failed.json`: RojoRHI's warning that a
  counter heap was refused means a scope went untimed and R did less work than the gate measured
  (issue #64). It follows ADR 0014 and runs on local hardware only.
- **Analysis** takes a process value as the median of its measured frames and the reported value as
  the median of the process values. The delta is the median of the paired per-process deltas R minus
  N, with a bootstrap interval of 10,000 resamples from a fresh `random.Random(20261018)` per
  comparison. A CPU miss is an interval off zero with a median delta above the larger of 5% of N and
  5 ns times `c` ([ADR 0025](../decisions/0025-cpu-budget-per-call-floor.md)); the 5% verdict is
  reported beside it, in the "5% rule" column, which is how rounds 1 and 2 read. The count rule uses
  the registered N column; a count varying across frames or processes is judged on its maximum. An
  allocation miss is R's median above N's in any one configuration; the allocations of C10 at
  `maxCount` 1 are reported only ([round 3](../research/2026-10-21-metal4-overhead-round3.md)).

The note's owner amended two of its rules on 2026-10-18, and the harness implements the amended
form: C8 registers a barrier between passes, with `c` 7 and K up to 10,000, and the composed frame's
depth and shadow barriers (rows 2 and 4) use `DepthStencilRead | DepthStencilWrite`. Round 3's
changes are on its page, linked above. `SHA256SUMS` covers every file under the output directory.

## State and what remains

The R arm runs on Null and on Metal 4. On `metal4` it loads each `Shaders/<name>.metal4lib` that
the Slang rule builds beside the executable, `RojoRHIBench/metal4` passes, and the driver's
untimed correctness gate (`bench.py build --backend metal4`, then `correctness`, R against N in
Release and Debug) passes every configuration, kept under the `t2-gate` topic so that its logs stay
out of the timed round's `t2` directory. The native arm runs on this Mac, and its correctness smoke
(`--arms N`) is recorded as `t2-native-smoke` evidence, the three runs `native-check`,
`native-build` and `native-gate` ([recording GPU evidence](../guides/gpu-evidence.md)).

The first timed round ran on 2026-10-19 under the `t2` topic and missed the CPU, GPU and allocation
budgets ([results](../research/2026-10-18-metal4-overhead-results.md)); a report-only diagnosis of
the misses is kept under `t2-diagnosis`. Round 2, registered by the note's amendment of 2026-10-20
([round 2](../research/2026-10-20-metal4-overhead-round2.md)), measured the fixed backend into
`t2/2026-10-20-round2/` ([round 2 results](
../research/2026-10-20-metal4-overhead-round2-results.md)). It met the GPU (−2.40% on the composed
frame), pipeline and native-call budgets, and missed CPU per operation in 15 of 34 judged
configurations (C1 to C3 at `K` 100, C2 and C3 above, C5-k10000, C6, and C1-k100000 narrowly) and
allocations in 57 of 63, by 2 to 7 a frame. Both rounds stand as judged at 5%; later rounds use
ADR 0025. The allocation misses remain defects for the next round.

The d3d12 harness is implemented and verified on WARP only, not on hardware. The `d3d12-bench` CI
job builds it in Debug (with the debug layer) and Release on WARP, runs `RojoRHIBench/host` in both
modes, `native` and `d3d12` in Debug, and the correctness gate (`bench.py build --backend d3d12
--adapter warp`, then `correctness`, which runs R and N in both modes) for every configuration but
`frame`, whose N arm is not built on d3d12 yet. The gate fails unless each selected workload's
`summary.json` entry has `passed` true and `gate` `compared`, and the job uploads
`build/bench-evidence/`. `bench.py timing` refuses a WARP build; D3D12 allocation counts are
recorded but no `[d3d12]` case judges them yet. Timing waits for the Windows GPU machine (issue
#21).

## Backend obligations the N arm encodes

What N encodes of the backends' lowering, and the overhead that R may legitimately add, is on
its own page: [backend obligations of the native arm](benchmarks-obligations.md).
