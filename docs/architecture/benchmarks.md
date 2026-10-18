# The Benchmark Harness

**Status**: Accepted

`Benchmarks/` builds the executable `RojoRHIBench` on macOS. It measures the overhead of RojoRHI
against native Metal 4 written in metal-cpp, as [the pre-registration](
../research/2026-10-18-metal4-overhead.md) specifies: that note is the authority on the workloads,
the metrics, the statistics and the decision rules, and this page does not restate its tables. It
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
link holds one copy of metal-cpp's tables. The target is built with
link-time optimization in Release, and `xmake/slang.lua` and the `rojorhi.metal` rule of
`xmake/metal.lua` write each target's generated headers and metallibs under that target's own
directory.

## Arms

A workload arm is a class built from its `A::Context` (`NativeContext` or `RojoContext`) and a
`Config` that creates everything it needs, records, submits and waits one frame in `frame(index)`,
and returns its outputs from `readback()`. One template, `runArm<A>`, drives the warm-up and the
measured frames and takes every measurement, so both arms are measured by the same code and no
virtual call sits in a timed span.
Each workload family registers its configurations through one function: `registerCpuWorkloads`,
`registerCountedWorkloads`, `registerKernelWorkloads` and `registerFrameWorkloads`.

The native arm (N) is the registered minimum of Metal 4 calls for the workload. The RojoRHI arm (R)
includes only `<rojoRHI/RHI.h>`, and records through `bench::List` and `bench::QueueRef`: aliases of
`CommandList` and `Queue` on a `metal4` build, tracing wrappers on a Null build. G2 has a third arm,
F, spike 2's fixed-binding variant; it runs after R and N in every process.

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

- **CPU span.** `mach_absolute_time`. R: `acquireCommandList` to `end`; N: `beginCommandBuffer` to
  `endCommandBuffer`. C9 records its `K` lists before the span, which times the `K` submit calls
  alone. The composed frame has no CPU span, so its `cpuNs` is null.
- **GPU span.** Every commit of both arms carries commit feedback, in every run. A frame's span is
  the earliest `GPUStartTime` to the latest `GPUEndTime` over its commits. A `FeedbackCollector`
  with preallocated slots receives them, and a frame is complete when every commit has reported.
  Neither arm adds a fence or an event the note does not register. On Null a submission completes
  inside the call and no GPU time exists.
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
| `[native]` | local Apple Silicon | N's outputs against CPU oracles for C1 to C10, G1 and G2, and the composed frame's determinism across frames |
| `[metal4]` | `metal4` builds | R's per-operation count is at most N's plus `c`, and N's equals the note's column, for C1 to C9 |

`xmake test RojoRHIBench/null` runs `[null],[host]`, `RojoRHIBench/native` runs `[native]` under
`MTL_DEBUG_LAYER=1`, and `RojoRHIBench/metal4` exists only with that backend. The `[metal4]` case is
compiled in every build. The interface CI job runs `null` in Debug and Release on macOS and requires
a non-zero `[native]` inventory, since hosted runners have no Metal 4 device.

## The commit timing seam

The Metal 4 backend implements `Backends/Metal4/Source/CommitTiming.h`, a private header that no
public header names. `rojoRHI::metal4::setCommitTiming(queue, callback, user)` makes every later
commit of that queue carry `MTL4::CommitOptions` feedback that calls `callback` from Metal's
completion thread with a `CommitTime` (queue, a per-queue serial counted from 1, start and end in
seconds, and a failure flag); a null callback detaches. Those calls are not counted in the device's
counters. The harness calls it from one file, `Benchmarks/Rojo/RojoGpuTiming.cpp`, under
`ROJORHI_BENCH_BACKEND_METAL4`; on Null the R arm reports no GPU time.

## The driver

`Tools/bench.py` runs the round and `Tools/bench_analysis.py` holds the statistics and the rules as
pure functions. Its commands take `--root`, `--topic` (default `t2`) and `--date`; the output is
`<root>/<topic>/<date>/`, the root defaulting to `rojo-rhi-evidence` beside the main checkout, also
when the driver runs from a git worktree ([recording GPU evidence](../guides/gpu-evidence.md)).

```
python3 Tools/bench.py build [--backend metal4|null]   builds Release and Debug, copies the binaries
                                                        and Shaders/ into bin/, writes
                                                        configurations.json and bench-environment.json,
                                                        returns xmake to Null Debug
python3 Tools/bench.py correctness [--arms R,N] [--only C1,G2]
python3 Tools/bench.py timing [--only C1,G2]
python3 Tools/bench.py analyze                         writes summary.json and analysis.txt
```

- **Correctness** runs each configuration twice, in Release without validation and in Debug with
  validation and `MTL_DEBUG_LAYER=1`, compares R's dumped bytes with N's, and fails on any standard
  error line other than the validation layer's banner. A configuration passes only when R and N were
  both compared; a run of one arm is a smoke. N's per-operation count must equal the registered
  column. On a Null build `--arms N` is required. A workload with any failing configuration is
  not timed.
- **Timing** refuses a Null build and battery power, removes `MTL_*`, `METAL_*` and `Malloc*`
  variables from the children's environment, skips results that exist (resume), renames a process
  whose thermal state was not nominal to `.discarded<k>.json`, rests 120 s before a retry, and marks
  the configuration not run after the fourth attempt. It follows ADR 0014 and runs on local hardware
  only.
- **Analysis** takes a process value as the median of its measured frames and the reported value as
  the median of the process values. The delta is the median of the paired per-process deltas R
  minus N, with a bootstrap interval of 10,000 resamples from a fresh `random.Random(20261018)` per
  comparison. The count rule uses the registered N column; a count that differs between frames or
  processes is reported unstable and judged on its maximum.

The note's owner amended two rules and the harness implements the amended form: C8 registers a
barrier between passes, with `c` 7 and K up to 10,000, and the composed frame's depth and shadow
barriers (rows 2 and 4) use `DepthStencilRead | DepthStencilWrite`. `SHA256SUMS` covers every file
under the output directory, recursively.

## State and what remains

The R arm runs on Null only. The [Metal 4 backend](metal4-backend.md) implements the device and
its resources but not yet the recording, pipeline and presentation entries the workloads call, nor
`CommitTiming.h`: on `metal4` the harness compiles, and its link lacks
`rojoRHI::metal4::setCommitTiming`. The native arm runs on this Mac, and its correctness smoke
(`--arms N`) is recorded as `t2-native-smoke` evidence, the three runs `native-check`,
`native-build` and `native-gate`
([recording GPU evidence](../guides/gpu-evidence.md)). Before a timed round:

- keep the obligations the N arm encodes (next section) in view when the backend lowers the R arm;
- implement `CommitTiming.h` and the entries the R arm calls in the Metal 4 backend, build
  `RojoRHIBench` on `metal4`, and run `RojoRHIBench/metal4` and the driver's full correctness gate;
- run `build`, `correctness`, `timing` and `analyze` and record the results in the note's Results
  section.

## Backend obligations the N arm encodes

N is the native minimum. Where R's lowering may legitimately do more, the difference is measured
overhead and not a defect of the harness. In particular:

- A barrier still pending at a list's end (the closing batches of the gbuffer and ao lists) lowers
  to no native barrier in N.
- N closes the compute encoder at a scope's end, while the design's M7 keeps it open until a render
  pass, an upscale or `end`; R may therefore emit an encoder barrier at the ao closing batch that N
  does not.
- `Copy` lowers to `StageBlit` only.
- Allocator resets are not native-call targets.
- C9's count window spans the same `K` submit calls as its timed span, but in R it sums only the
  deltas of `nativeSubmitCalls` and `nativeHostCalls`: the lists' record calls and barriers, which
  fold into the counters at submission, are left out. Any submit-time encoding the backend does
  must therefore count as submit calls.
- `CommitTiming.h` promises exactly one commit per `Queue::submit` and no backend-internal commit on
  a queue while timing is attached to it: `RojoGpuTiming` fails a frame on a stray or
  non-contiguous serial. The seam's feedback must also coexist with a commit feedback handler the
  backend sets for its own waits.
