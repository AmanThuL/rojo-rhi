# ADR 0014: Overhead is measured against native code, to budgets fixed in advance

**Status**: Accepted (2026-10-12); its per-operation CPU budget superseded by
[ADR 0025](0025-cpu-budget-per-call-floor.md)

## Context

A thin RHI is justified by what it costs relative to calling Metal 4 or D3D12 directly. Comparing
two candidate designs with each other alone lets a weak incumbent make anything look good: the
consumer's earlier address-first experiment (Luminex ADR 0010, "The RHI adopts an address-first
per-frame data path; the object model stays") found its largest CPU win came from defects in the
incumbent, while several of its losses were properties of the prototype rather than of the model.
A whole-API prototype also changes binding, barriers, allocation and pipelines at once, so a
result cannot be attributed to a choice. The measurement part of the language and measurement
research note surveys how RHIs measure abstraction overhead:
[measurement](../research/2026-10-11-rhi-redesign/07-language-and-measurement-part3-measurement.md).

## Decision

Overhead is measured against hand-written native code, decided one variable at a time, counted
exactly before it is timed, swept across scale, and judged against budgets fixed before any
measurement runs.

### Principles

1. **Native baselines.** Overhead only means something relative to the best hand-written Metal 4
   or D3D12 code for the same workload. Each backend gets a small native baseline per workload, and
   every candidate is scored as a tax over it: CPU nanoseconds per operation, GPU frame time, and
   native calls emitted beyond the minimum.
2. **One variable per experiment.** Each contested decision gets its own spike, holding everything
   else at native. Interacting decisions get a small factorial experiment.
3. **Counts before timings.** Native calls, allocations, hash lookups, descriptor writes, barriers,
   pipelines and bytes uploaded are exact, machine-independent and usable in CI. Timings confirm
   that a count difference matters.
4. **Scaling sweeps.** Draw count, binding churn, pass count and barrier density are swept from one
   to about 100,000. The failures that matter are cliffs (an O(n) scan, a ring that overflows) that
   a single representative frame hides until it crosses them.
5. **GPU tax measured separately.** Extra shader indirection appears in GPU time, not CPU time. It
   is measured with GPU timestamps on kernel microbenchmarks, with indices uniform across a wave
   and divergent per thread.
6. **One composed frame.** A fixed mini-renderer of ten to fifteen passes (shadows, geometry
   buffer, asynchronous compute, post-processing) is written natively and through RojoRHI on both
   backends. It confirms that the parts compose and yields the headline overhead.
7. **Pre-registration.** Budgets, workloads, statistics and decision rules are fixed before
   measurement. Changing instrumentation mid-round invalidates the round.

### Protocol

- Release builds, validation off for timed runs, validation on for correctness runs.
- Paired, interleaved runs in one fresh process per repetition, alternating candidate order.
- Warm-up frames, then a fixed count of measured frames; medians of repetition medians.
- Two-sided 95% bootstrap confidence intervals on paired deltas.
- Environment recorded per run: hardware, OS build, toolchain, driver, power source, thermal state.
- Gates come first (correct output, validation clean, an honest mapping), then overhead against
  budget, then interface ergonomics as the tie-break: concept and function counts and the lines of
  code for a fixed sample set.

### Budgets

| Metric | Budget |
|---|---|
| CPU tax per operation over native | at most 5% |
| GPU frame-time tax over native, composed frame | at most 1% |
| Hidden pipeline variants | zero on every backend |
| Per-frame heap allocations after warm-up | zero |
| Native calls per RojoRHI call on hot paths | at most the native minimum plus one |

A difference counts as material when the confidence interval excludes zero and the effect clears
the budget. Ties keep the simpler design.

### Pre-registered spikes

| Spike | Question | Candidates | Deciding metrics |
|---|---|---|---|
| 1 | Is `Addr` dereference on D3D12 cheap enough for hot paths? | root fields only; one `Addr` hop; two hops; uniform and divergent bases | GPU kernel tax, shader instruction count |
| 2 | How does Metal 4 index textures? | CPU-written 8-byte ID table; contiguous view-pool `base + i` | GPU kernel tax, CPU write cost, documented-behaviour gate |
| 3 | How does Metal 4 run multi-draw indirect with a GPU count? | backend kernel encoding an indirect command buffer; `maxCount` loop of indirect draws with early-out | GPU time across draw counts, CPU encode time, memory |

These three decisions are not settled by analysis, which is why they are pre-registered. Spike
results are folded into the API reference before the corresponding chapter is accepted. Each spike
lives on a `spike/<question>` branch and records its result; production work follows on a clean
branch.

### Where measurement runs

| Question | Machine |
|---|---|
| Metal 4 CPU and GPU costs | the Apple Silicon Mac |
| D3D12 CPU and GPU costs, x86 and MSVC code generation, Control Flow Guard on indirect calls | the Windows 11 GPU machine |
| Counts, interface surface, compile-time cost | hosted CI through the Null backend |

### Continuous guard

After the redesign the same microbenchmarks and counters become a regression suite. Hosted CI
asserts counters exactly on the Null backend. Before merge, a quick paired run against the merge
base on the Mac (and on Windows for D3D12 changes) fails when the upper confidence bound of a
slowdown exceeds 5%. At each minor version the full protocol runs on both machines and its report
is kept outside the published tree.

## Consequences

- Budgets are fixed here, so a result is judged against a number written before the measurement;
  changing a budget is a superseding ADR, not a note in a report.
- The D3D12 rows of every measurement wait for a Windows GPU machine and a D3D12 backend. Until
  then the D3D12 side of spike 1 has no measurement, and the reference recommends root fields and
  heap indices for hot data ([0011](0011-shader-abi.md)).
- The benchmark tier of the repository's test suite is this suite: it runs on both machines, with
  counters asserted on hosted CI.
- A spike whose result contradicts the current reference text changes the reference in the same
  change that records the result.
