# The Metal 4 backend's overhead against native code: round 3 results

**Status**: Accepted (2026-10-21)

These are the results of round 3, which [the round-3 amendment](
2026-10-21-metal4-overhead-round3.md) registered under [the pre-registration](
2026-10-18-metal4-overhead.md), judged by that note's decision rules as amended, with CPU time per
operation judged by [ADR 0025](../decisions/0025-cpu-budget-per-call-floor.md). Rounds 1 and 2
([round 1](2026-10-18-metal4-overhead-results.md), [round 2](
2026-10-20-metal4-overhead-round2-results.md)) stay as they were recorded, judged at 5%; the note's
Results section summarises every round, and this page holds every row of round 3.

## What changed since round 2

Round 2 ran at 0a0b53f; round 3 measures the backend after the fixes to round 2's misses. Its
amendment states everything that changed, with the numbers known when each change was decided: CPU
per operation is judged by ADR 0025, a miss being an interval excluding zero and a median delta
above the larger of 5% of N and 5 ns times `c`, with the 5% verdict reported beside it; the
allocations of C10 at `maxCount` 1 (C10m1) are reported only; N signals and waits on its own frame
event, as R does; R's `DeviceDesc` sets `scopeTimings` only in C8 and the composed frame, where N
writes timestamps too; and the 32 commits on `main` since round 2 that touch the arms' code are
listed there with their effect.

## Measurement

Measured on 2026-10-21 from 19:45 to 23:38 CDT on an Apple M3 Max (Apple7 to Apple9, Metal 4,
timestamps at 24 MHz) under macOS 27.0.1 (26A434), Xcode 26.6, `metal` 32023.883, Apple clang
21.0.0 and `metal-cpp` c595afe, on AC power, from a clean checkout of 378f219a, the tip of `main`
once the amendment's pull request had landed. `bench.py build` finished at 19:45, and the
correctness gate ran at that commit from 19:46 to 19:47, before anything was timed, and passed every
workload in Release and in Debug. Timing ran from 19:49 to 23:38, one workload batch at a time, and
`analyze` followed. All 1,260 timed processes (63 configurations of 20) started and ended at the
nominal thermal state on their first attempt: none was discarded, no configuration is reported not
run, and no process printed a line on standard error, so the round was not stopped and no scope
went untimed. Raw results, logs, the driver's output, the load logs below, `summary.json`,
`analysis.txt` and a SHA-256 manifest are kept outside the published tree, under
`rojo-rhi-evidence/t2/2026-10-21-round3/`. The manifest's entry for the driver's own
`load/driver-analyze.out` is the empty file's hash, taken before the driver's redirect wrote it, as
in round 2; its written hash is beside it in `load/driver-analyze.out.sha256`, and every other
entry verifies.

## Load

Before the correctness gate and before each workload batch, a check waited until no build,
compiler, test or bench process and no foreign process above 30% of a core had run for 60 s, and
recorded the load; the gate and all 14 batches started after such a quiet minute. During the round
a watcher logged every 30 s any process above 30% of a core. Checks held batches back for seconds
to minutes several times (`load-checks.log.waits`); the two long holds kept these events out of
every batch: a FaceTime call (FaceTime up to 178% of a core, `avconferenced` 30%
to 101%) from 20:16:08 to 20:36:53, after C5 ended (20:13:37), then `MenuBarAgent` and
`PerfPowerServices`, held C6 to 20:39:35; and Dropbox's
updater (66% to 123%, with `corespotlightd` at 162% in one sample) from 23:23:56 to 23:25:34, after
C8 ended (23:23:04), held C9 to 23:26:39. Against each batch's timing window, from its first to its
last result file, the processes the watcher logged that were not the desktop's own:

| Time | Process | Batch at that time |
|---|---|---|
| 20:01:33 | `gh run download` of a CI run (39698810890) by another session on the machine, 93% of one core, one sample | C3 (19:59:11 to 20:03:59), during process 9 of C3-k100000; C3-k100's processes 0 to 9 had ended before the sample, and 10 to 19 ended after it |
| 20:13:37 | `Tools/check_project_policy.py`, not part of the round, 48% (99% in the next check's sample at 20:13:38) | C5 (20:13:14 to 20:13:37), at its end; the sample at 20:13:07 did not show it, so how much of C5 it overlaps is not known |
| 22:05:10, 22:12:42, 23:04:58 | a `claude` command-line process, 31% to 53%, one sample each | C8 (21:13:36 to 23:23:04), during processes 7, 9 and 17 of C8-k10000 |
| 22:10:42 | `gh run download` of the D3D12 benchmark gate's CI run (39711745231) by another session, 94%, one sample | C8, during process 8 of C8-k10000 |

The desktop's own processes were logged above 30% of a core in every batch but C5, C9, C10m1, G1,
G2 and the composed frame's, among them Spotlight (`mds_stores`, `corespotlightd`: 133% at
20:10:36 in C4, 42% to 125% from 21:59:39 to 22:03:40 in C8), `spindump` (79% to 91% from 20:44:17
to 20:44:47 in C6), `mediaanalysisd`, `duetexpertd`, iCloud's `bird`,
`IntelligencePlatformComputeService` (95% at 21:59:08 in C8), `PerfPowerServices` (131% at 23:13:00
in C8), `textunderstandingd`, browser and app renderers and an update assistant. The registered
process checks, the thermal discard and the stop on a line on standard error, did not fire, so no
process was discarded and the results stand as registered; nothing is rerun, since the note allows
no selective rerun. The download is the only foreign process logged during C3, whose `K` 100 misses
below. C3-k100's paired deltas are 12.50 to 19.58 ns across its 20 processes, with a median of
14.17 ns over processes 0 to 9 and 13.33 ns over 10 to 19; the largest, 19.58 ns, is process 11,
which ended at 20:01:50, between the sample that saw the download and the next, which did not
(reported only).

## Verdict

The round misses one of the five budgets, CPU per operation, in one of 34 judged configurations,
C3-k100; round 2 missed two, CPU and allocations. Judged by the note's original 5% rule, which round
3 reports and does not decide by, CPU would miss in four, C3 at every judged `K`.

| Budget | Round 2 (2026-10-20) | Round 3 | The 5% rule on round 3 (reported) |
|---|---|---|---|
| CPU tax per operation: round 2 at most 5%; round 3 at most the larger of 5% of N and 5 ns times `c` | **missed**: 15 of 34 judged configurations | **missed**: 1 of 34, C3-k100: R 146.67 ns, N 132.92 ns, delta 13.96 ns [13.33, 14.17] against a budget of 10.00 ns, +10.50%. Passes: every other judged configuration, among them C3 at `K` 1,000 to 100,000 (7.90 to 8.57 ns against 10.00), C6-k100 (5.83 against 10.00) and C10-k100 (37.92 against 45.70) | would miss 4 of 34: C3 at every judged `K`, k100 +10.50%, k1000 +10.41%, k10000 +12.14%, k100000 +13.15%. Every other judged configuration is within 5%, at most +4.60% (C6-k100) |
| GPU frame-time tax, composed frame, at most 1% | met: −46.9 µs [−59.0, −38.5], −2.40% | met: R 1,920.70 µs, N 1,962.90 µs, −43.25 µs [−50.79, −37.59], −2.20% | - |
| Hidden pipeline variants, zero | met | met: no pipeline and no native creation after warm-up in any process of any configuration | - |
| Per-frame heap allocations, R at most N | **missed**: R above N in 57 of 63 | met in every judged configuration: R equals N on C1 to C7, C9, C10, G1 and G2, and is below it on C8 (by 3 to 29,980) and the composed frame (157 against 174); RojoRHI's own `cpuAllocations` is 0 everywhere. C10m1, reported only: R above N by 1 at every `K` (15 against 14, 16 against 15 at `K` 1,000), which the amendment attributes to the counted-draw kernels' compute encoder | - |
| Native calls, at most N plus `c` | met: C8 13 against a limit of 19, C9 2 against 3 | met: every operation of C1 to C9 is within, with stable counts; C8 13 against a limit of 19, C9 2 against 3; C10 3 against N's 3 and C10m1 1 against N's 1, reported | - |

## Rows

**CPU and GPU time**, per configuration: CPU nanoseconds per operation and GPU microseconds per
frame, the median of process medians for R and N, and the median of the paired deltas R minus N with
its 95% bootstrap interval. The relative column is the delta over N's median. CPU is judged at `K`
of 100 and above for C1 to C10, against the budget column (ADR 0025), with the 5% verdict beside
it; GPU is judged on the composed frame only; every other cell is reported. C10m1 is reported only.
The composed frame has no CPU span. Values are those of `analysis.txt`, GPU converted from ns.

| Id | CPU R, ns/op | CPU N | Delta [95% interval] | Rel. | Budget, ns | CPU rule | 5% rule | GPU R, µs | GPU N | Delta, µs [95% interval] | Rel. | GPU rule |
|---|---:|---:|---|---:|---:|---|---|---:|---:|---|---:|---|
| C1-k1 | 6,125.00 | 5,750.00 | 416.67 [354.17, 458.33] | +7.25% | - | reported | - | 13.58 | 13.58 | 0.05 [-0.06, 0.14] | +0.38% | reported |
| C1-k10 | 712.50 | 670.83 | 43.75 [38.54, 50.00] | +6.52% | - | reported | - | 13.30 | 13.58 | -0.16 [-0.49, -0.07] | -1.15% | reported |
| C1-k100 | 143.12 | 138.54 | 4.17 [2.92, 6.15] | +3.01% | 15.00 | pass | pass | 16.62 | 16.54 | 0.08 [-0.03, 0.14] | +0.50% | reported |
| C1-k1000 | 76.68 | 74.83 | 1.89 [1.52, 2.07] | +2.52% | 15.00 | pass | pass | 63.27 | 63.32 | -0.05 [-0.75, 0.30] | -0.08% | reported |
| C1-k10000 | 67.19 | 65.36 | 2.13 [1.36, 2.36] | +3.26% | 15.00 | pass | pass | 156.50 | 157.52 | -1.42 [-3.08, -0.01] | -0.90% | reported |
| C1-k100000 | 66.70 | 64.73 | 1.71 [1.01, 2.41] | +2.64% | 15.00 | pass | pass | 1,190.34 | 1,168.97 | 19.06 [15.57, 23.71] | +1.63% | reported |
| C2-k1 | 6,145.83 | 5,708.33 | 416.67 [416.67, 458.33] | +7.30% | - | reported | - | 13.75 | 13.61 | 0.14 [0.01, 0.19] | +0.99% | reported |
| C2-k10 | 716.67 | 670.83 | 50.00 [37.50, 56.25] | +7.45% | - | reported | - | 13.20 | 13.26 | -0.11 [-0.29, 0.18] | -0.86% | reported |
| C2-k100 | 143.96 | 139.58 | 4.38 [3.54, 5.42] | +3.13% | 15.00 | pass | pass | 15.95 | 15.86 | 0.13 [-0.03, 0.25] | +0.79% | reported |
| C2-k1000 | 77.22 | 76.00 | 1.56 [1.22, 1.83] | +2.06% | 15.00 | pass | pass | 62.31 | 62.57 | -0.36 [-1.16, 0.68] | -0.58% | reported |
| C2-k10000 | 69.50 | 67.16 | 1.81 [0.88, 2.49] | +2.69% | 15.00 | pass | pass | 157.93 | 159.79 | -1.21 [-3.07, 1.30] | -0.76% | reported |
| C2-k100000 | 68.04 | 66.11 | 2.53 [0.85, 2.97] | +3.82% | 15.00 | pass | pass | 1,208.89 | 1,190.62 | 21.94 [18.47, 23.84] | +1.84% | reported |
| C3-k1 | 6,208.33 | 5,750.00 | 375.00 [364.58, 500.00] | +6.52% | - | reported | - | 13.63 | 13.63 | 0.05 [-0.04, 0.20] | +0.38% | reported |
| C3-k10 | 705.21 | 654.17 | 50.00 [45.83, 57.29] | +7.64% | - | reported | - | 13.65 | 13.19 | 0.42 [0.16, 0.69] | +3.16% | reported |
| C3-k100 | 146.67 | 132.92 | 13.96 [13.33, 14.17] | +10.50% | 10.00 | **miss** | **miss** | 18.68 | 18.62 | 0.03 [-0.26, 0.30] | +0.17% | reported |
| C3-k1000 | 84.09 | 76.33 | 7.95 [7.65, 8.27] | +10.41% | 10.00 | pass | **miss** | 94.50 | 94.38 | 0.05 [-0.04, 0.14] | +0.06% | reported |
| C3-k10000 | 72.99 | 65.08 | 7.90 [7.72, 8.18] | +12.14% | 10.00 | pass | **miss** | 406.77 | 436.26 | -27.38 [-32.00, -1.43] | -6.27% | reported |
| C3-k100000 | 73.73 | 65.12 | 8.57 [8.27, 9.70] | +13.15% | 10.00 | pass | **miss** | 4,915.11 | 4,911.57 | 4.89 [1.83, 8.28] | +0.10% | reported |
| C4-k1 | 3,291.67 | 3,125.00 | 208.33 [125.00, 250.00] | +6.67% | - | reported | - | 2.54 | 2.43 | 0.08 [0.08, 0.29] | +3.44% | reported |
| C4-k10 | 441.67 | 412.50 | 25.00 [20.83, 31.25] | +6.06% | - | reported | - | 3.88 | 3.79 | 0.19 [0.08, 0.32] | +4.94% | reported |
| C4-k100 | 116.77 | 116.88 | 0.00 [-1.25, 2.60] | +0.00% | 15.00 | pass | pass | 24.17 | 24.15 | 0.06 [-0.02, 0.12] | +0.26% | reported |
| C4-k1000 | 79.33 | 78.72 | 0.12 [-0.46, 1.29] | +0.16% | 15.00 | pass | pass | 242.64 | 242.59 | 0.07 [-0.26, 0.21] | +0.03% | reported |
| C4-k10000 | 70.64 | 71.20 | 0.42 [-1.75, 1.28] | +0.59% | 15.00 | pass | pass | 2,297.52 | 2,291.18 | 5.11 [0.43, 14.62] | +0.22% | reported |
| C4-k100000 | 70.96 | 71.25 | -0.60 [-2.01, 2.17] | -0.84% | 15.00 | pass | pass | 17,624.19 | 17,585.49 | 12.01 [-62.25, 55.26] | +0.07% | reported |
| C5-k1 | 916.67 | 916.67 | 0.00 [-41.67, 41.67] | +0.00% | - | reported | - | 0.00 | 0.00 | 0.00 [0.00, 0.00] | - | reported |
| C5-k10 | 95.83 | 98.96 | 0.00 [-4.17, 2.08] | +0.00% | - | reported | - | 0.00 | 0.00 | 0.00 [0.00, 0.00] | - | reported |
| C5-k100 | 14.17 | 14.17 | 0.00 [-1.25, 0.83] | +0.00% | 5.00 | pass | pass | 0.00 | 0.00 | 0.00 [0.00, 0.00] | - | reported |
| C5-k1000 | 5.71 | 6.03 | -0.34 [-0.98, 0.17] | -5.70% | 5.00 | pass | pass | 0.00 | 0.00 | 0.00 [0.00, 0.00] | - | reported |
| C5-k10000 | 5.10 | 5.06 | 0.03 [-0.08, 0.13] | +0.64% | 5.00 | pass | pass | 0.00 | 0.00 | 0.00 [0.00, 0.00] | - | reported |
| C5-k100000 | 4.40 | 4.36 | 0.04 [0.03, 0.05] | +0.98% | 5.00 | pass | pass | 0.00 | 0.00 | 0.00 [0.00, 0.00] | - | reported |
| C6-k1 | 4,083.33 | 3,927.08 | 145.83 [62.50, 208.33] | +3.71% | - | reported | - | 4.13 | 4.26 | -0.13 [-0.22, -0.06] | -2.93% | reported |
| C6-k10 | 500.00 | 475.00 | 20.83 [16.67, 29.17] | +4.39% | - | reported | - | 18.87 | 18.87 | 0.00 [-0.17, 0.28] | +0.00% | reported |
| C6-k100 | 132.92 | 126.88 | 5.83 [5.31, 6.46] | +4.60% | 10.00 | pass | pass | 166.48 | 165.38 | 0.76 [-0.43, 2.94] | +0.46% | reported |
| C6-k1000 | 112.21 | 109.90 | 2.08 [1.58, 2.76] | +1.90% | 10.00 | pass | pass | 1,570.89 | 1,567.36 | 5.20 [-2.11, 7.93] | +0.33% | reported |
| C6-k10000 | 77.49 | 74.65 | 2.82 [2.63, 2.91] | +3.78% | 10.00 | pass | pass | 15,701.93 | 15,674.97 | 2.17 [-20.94, 41.86] | +0.01% | reported |
| C6-k100000 | 80.57 | 77.79 | 2.66 [2.24, 3.16] | +3.42% | 10.00 | pass | pass | 124,602.33 | 126,015.47 | -1,849.98 [-2,966.96, -1,073.20] | -1.47% | reported |
| C7-k1 | 3,625.00 | 3,541.67 | 83.33 [62.50, 125.00] | +2.35% | - | reported | - | 2.79 | 2.69 | 0.12 [0.09, 0.13] | +4.65% | reported |
| C7-k10 | 483.33 | 470.83 | 12.50 [8.33, 16.67] | +2.65% | - | reported | - | 4.13 | 4.12 | 0.00 [0.00, 0.07] | +0.00% | reported |
| C7-k100 | 146.88 | 144.17 | 2.60 [1.25, 2.92] | +1.81% | 7.21 | pass | pass | 24.73 | 24.66 | 0.06 [-0.13, 0.28] | +0.25% | reported |
| C7-k1000 | 101.32 | 100.75 | 0.51 [-0.19, 1.11] | +0.51% | 5.04 | pass | pass | 248.42 | 248.28 | -0.22 [-1.68, 0.52] | -0.09% | reported |
| C7-k10000 | 92.87 | 92.11 | 0.96 [0.16, 1.17] | +1.04% | 5.00 | pass | pass | 2,366.11 | 2,364.19 | 1.98 [-1.42, 7.01] | +0.08% | reported |
| C7-k100000 | 94.54 | 93.83 | 0.75 [0.41, 0.94] | +0.80% | 5.00 | pass | pass | 18,253.22 | 18,133.77 | 126.81 [38.79, 199.16] | +0.70% | reported |
| C8-k1 | 6,208.33 | 9,416.67 | -3,135.42 [-3,333.33, -3,000.00] | -33.30% | - | reported | - | 13.77 | 31.94 | -18.00 [-20.77, -17.68] | -56.36% | reported |
| C8-k10 | 6,168.75 | 8,081.25 | -1,970.83 [-2,285.42, -1,846.88] | -24.39% | - | reported | - | 315.79 | 693.45 | -375.01 [-385.77, -368.32] | -54.08% | reported |
| C8-k100 | 6,537.29 | 15,062.71 | -9,957.40 [-12,393.02, -8,751.56] | -66.11% | 753.14 | pass | pass | 4,406.97 | 7,806.59 | -3,372.55 [-4,012.72, -3,181.91] | -43.20% | reported |
| C8-k1000 | 10,127.40 | 17,604.43 | -6,365.95 [-7,931.91, -5,947.86] | -36.16% | 880.22 | pass | pass | 43,359.22 | 87,055.64 | -40,309.40 [-49,072.46, -34,975.45] | -46.30% | reported |
| C8-k10000 | 6,621.31 | 8,879.29 | -2,631.18 [-2,987.56, -2,320.43] | -29.63% | 443.96 | pass | pass | 427,261.26 | 777,636.02 | -346,715.98 [-355,736.03, -319,217.92] | -44.59% | reported |
| C9-k1 | 1,958.33 | 2,125.00 | -208.33 [-208.33, -166.67] | -9.80% | - | reported | - | 2.95 | 2.63 | 0.32 [0.12, 1.90] | +12.30% | reported |
| C9-k10 | 913.54 | 893.75 | 14.58 [4.17, 25.00] | +1.63% | - | reported | - | 270.22 | 265.29 | 4.32 [3.48, 6.07] | +1.63% | reported |
| C9-k100 | 651.67 | 640.00 | 14.27 [-12.50, 36.46] | +2.23% | 32.00 | pass | pass | 3,321.37 | 3,206.93 | 106.60 [93.46, 143.30] | +3.32% | reported |
| C10-k1 | 9,125.00 | 8,416.67 | 750.00 [697.92, 791.67] | +8.91% | - | reported | - | 77.18 | 75.19 | 1.30 [-1.19, 4.60] | +1.73% | reported |
| C10m1-k1 | 7,833.33 | 5,562.50 | 2,270.83 [2,208.33, 2,291.67] | +40.82% | - | reported | - | 32.85 | 12.75 | 20.13 [19.58, 20.51] | +157.84% | reported |
| C10-k10 | 4,375.00 | 4,148.96 | 300.00 [167.71, 350.00] | +7.23% | - | reported | - | 272.37 | 272.70 | -0.68 [-1.82, 0.61] | -0.25% | reported |
| C10m1-k10 | 875.00 | 627.08 | 243.75 [231.25, 252.08] | +38.87% | - | reported | - | 33.18 | 11.63 | 21.55 [21.12, 21.71] | +185.39% | reported |
| C10-k100 | 954.27 | 913.96 | 37.92 [33.85, 40.94] | +4.15% | 45.70 | pass | pass | 1,673.79 | 1,679.72 | -8.65 [-19.15, -3.16] | -0.51% | reported |
| C10m1-k100 | 149.17 | 111.46 | 37.81 [36.98, 38.75] | +33.93% | - | reported | - | 46.38 | 14.48 | 31.84 [31.56, 32.29] | +219.93% | reported |
| C10-k1000 | 599.83 | 598.41 | 0.28 [-5.73, 10.10] | +0.05% | 29.92 | pass | pass | 16,952.50 | 16,956.89 | -10.24 [-70.64, 13.60] | -0.06% | reported |
| C10m1-k1000 | 72.59 | 60.30 | 12.26 [11.79, 12.73] | +20.33% | - | reported | - | 133.24 | 59.80 | 73.56 [73.02, 73.77] | +123.01% | reported |
| G1-uniform | 3,250.00 | 3,000.00 | 239.58 [208.33, 270.83] | +7.99% | - | reported | - | 62.10 | 50.98 | 11.14 [9.94, 12.19] | +21.84% | reported |
| G1-divergent | 8,885.42 | 8,250.00 | 718.75 [583.33, 802.08] | +8.71% | - | reported | - | 430.23 | 425.75 | 4.54 [3.97, 4.70] | +1.07% | reported |
| G2-uniform | 3,343.75 | 3,083.33 | 250.00 [229.17, 322.92] | +8.11% | - | reported | - | 62.25 | 62.12 | 0.14 [0.06, 0.25] | +0.22% | reported |
| G2-divergent | 9,187.50 | 8,510.42 | 645.83 [562.50, 854.17] | +7.59% | - | reported | - | 1,541.98 | 1,541.65 | 0.25 [0.21, 0.36] | +0.02% | reported |
| frame | - | - | - | - | - | reported | - | 1,920.70 | 1,962.90 | -43.25 [-50.79, -37.59] | -2.20% | pass |

G2's arm F, for the record: 57.29 µs against N's 62.12 µs uniform (−4.87 µs [−4.98, −4.71],
−7.85%) and 1,540.35 against 1,541.65 µs divergent (−1.35 µs [−1.38, −1.25], −0.09%).

**Native calls per repetition**, `(count at K = 1,000 − count at K = 100) / 900`, for C9 over `K`
100 and 10. Every count was the same in every measured frame of every process.

| Workload | R | N | Limit, N plus `c` | `c` | Rule |
|---|---:|---:|---:|---:|---|
| C1 | 2 | 2 | 5 | 3 | pass |
| C2 | 2 | 2 | 5 | 3 | pass |
| C3 | 4 | 4 | 6 | 2 | pass |
| C4 | 2 | 2 | 5 | 3 | pass |
| C5 | 0 | 0 | 1 | 1 | pass |
| C6 | 2 | 2 | 4 | 2 | pass |
| C7 | 1 | 1 | 2 | 1 | pass |
| C8 | 13 | 12 | 19 | 7 | pass |
| C9 | 2 | 2 | 3 | 1 | pass |
| C10 | 3 | 3 | - | 1 | reported |
| C10 at `maxCount` 1 | 1 | 1 | - | - | reported |

**Allocations per frame** through the default malloc zone, the median of process medians, and
RojoRHI's own `cpuAllocations` for R. C10m1's rows are reported only (the amendment).

| Id | Allocations R | N | Delta [95% interval] | R `cpuAllocations` | Rule |
|---|---:|---:|---|---:|---|
| C1-k1 | 14 | 14 | 0 [0, 0] | 0 | pass |
| C1-k10 | 14 | 14 | 0 [0, 0] | 0 | pass |
| C1-k100 | 15 | 15 | 0 [0, 0] | 0 | pass |
| C1-k1000 | 15 | 15 | 0 [0, 0] | 0 | pass |
| C1-k10000 | 18 | 18 | 0 [0, 0] | 0 | pass |
| C1-k100000 | 22 | 22 | 0 [0, 0] | 0 | pass |
| C2-k1 | 14 | 14 | 0 [0, 0] | 0 | pass |
| C2-k10 | 14 | 14 | 0 [0, 0] | 0 | pass |
| C2-k100 | 15 | 15 | 0 [0, 0] | 0 | pass |
| C2-k1000 | 15 | 15 | 0 [0, 0] | 0 | pass |
| C2-k10000 | 18 | 18 | 0 [0, 0] | 0 | pass |
| C2-k100000 | 22 | 22 | 0 [0, 0] | 0 | pass |
| C3-k1 | 15 | 15 | 0 [0, 0] | 0 | pass |
| C3-k10 | 15 | 15 | 0 [0, 0] | 0 | pass |
| C3-k100 | 15 | 15 | 0 [0, 0] | 0 | pass |
| C3-k1000 | 16 | 16 | 0 [0, 0] | 0 | pass |
| C3-k10000 | 18 | 18 | 0 [0, 0] | 0 | pass |
| C3-k100000 | 24 | 24 | 0 [0, 0] | 0 | pass |
| C4-k1 | 13 | 13 | 0 [0, 0] | 0 | pass |
| C4-k10 | 13 | 13 | 0 [0, 0] | 0 | pass |
| C4-k100 | 13 | 13 | 0 [0, 0] | 0 | pass |
| C4-k1000 | 14 | 14 | 0 [0, 0] | 0 | pass |
| C4-k10000 | 16 | 16 | 0 [0, 0] | 0 | pass |
| C4-k100000 | 21 | 21 | 0 [0, 0] | 0 | pass |
| C5-k1 | 12 | 12 | 0 [0, 0] | 0 | pass |
| C5-k10 | 12 | 12 | 0 [0, 0] | 0 | pass |
| C5-k100 | 12 | 12 | 0 [0, 0] | 0 | pass |
| C5-k1000 | 12 | 12 | 0 [0, 0] | 0 | pass |
| C5-k10000 | 12 | 12 | 0 [0, 0] | 0 | pass |
| C5-k100000 | 12 | 12 | 0 [0, 0] | 0 | pass |
| C6-k1 | 16 | 16 | 0 [0, 0] | 0 | pass |
| C6-k10 | 16 | 16 | 0 [0, 0] | 0 | pass |
| C6-k100 | 16 | 16 | 0 [0, 0] | 0 | pass |
| C6-k1000 | 17 | 17 | 0 [0, 0] | 0 | pass |
| C6-k10000 | 19 | 19 | 0 [0, 0] | 0 | pass |
| C6-k100000 | 24 | 24 | 0 [0, 0] | 0 | pass |
| C7-k1 | 16 | 16 | 0 [0, 0] | 0 | pass |
| C7-k10 | 16 | 16 | 0 [0, 0] | 0 | pass |
| C7-k100 | 16 | 16 | 0 [0, 0] | 0 | pass |
| C7-k1000 | 17 | 17 | 0 [0, 0] | 0 | pass |
| C7-k10000 | 20 | 20 | 0 [0, 0] | 0 | pass |
| C7-k100000 | 24 | 24 | 0 [0, 0] | 0 | pass |
| C8-k1 | 17 | 20 | -3 [-3, -3] | 0 | pass |
| C8-k10 | 54 | 75 | -21 [-21, -21] | 0 | pass |
| C8-k100 | 415 | 654 | -239 [-239, -239] | 0 | pass |
| C8-k1000 | 4,021 | 6,965 | -2,944 [-2,944, -2,944] | 0 | pass |
| C8-k10000 | 40,030 | 70,010 | -29,980 [-29,980, -29,979] | 0 | pass |
| C9-k1 | 18 | 18 | 0 [0, 0] | 0 | pass |
| C9-k10 | 154 | 154 | 0 [0, 0] | 0 | pass |
| C9-k100 | 1,514 | 1,514 | 0 [-0.5, 0] | 0 | pass |
| C10-k1 | 16 | 16 | 0 [0, 0] | 0 | pass |
| C10m1-k1 | 15 | 14 | +1 [+1, +1] | 0 | reported |
| C10-k10 | 16 | 16 | 0 [0, 0] | 0 | pass |
| C10m1-k10 | 15 | 14 | +1 [+1, +1] | 0 | reported |
| C10-k100 | 16 | 16 | 0 [0, 0] | 0 | pass |
| C10m1-k100 | 15 | 14 | +1 [+1, +1] | 0 | reported |
| C10-k1000 | 19 | 19 | 0 [0, 0] | 0 | pass |
| C10m1-k1000 | 16 | 15 | +1 [+1, +1] | 0 | reported |
| G1-uniform | 13 | 13 | 0 [0, 0] | 0 | pass |
| G1-divergent | 13 | 13 | 0 [0, 0] | 0 | pass |
| G2-uniform | 13 | 13 | 0 [0, 0] | 0 | pass |
| G2-divergent | 13 | 13 | 0 [0, 0] | 0 | pass |
| frame | 157 | 174 | -17 [-17, -17] | 0 | pass |

**Pipelines.** No pipeline was created and no native creation call was made after warm-up in any
process of any configuration.

**Departures during the round.** None in the code or the instrumentation: R's per-operation counts
are those the amendment states (C1, C2 and C4 2, C3 4, C5 0, C6 2, C7 1, C8 13, C9 2, C10 3, C10m1
1). The departures from the procedure are the load events above.

## The residual: C3

C3 binds one of two pipelines, alternating, then draws: `c` 2, so its budget is 10 ns at every `K`,
against 5% of N's 3.25 to 6.65 ns. Per operation, in nanoseconds:

| `K` | Round 2: N, delta | Informal profile at c11fbdad: N, delta [upper bound] | Round 3: R, N | Round 3: delta [95% interval] | 5% of N | ADR 0025 | 5% rule |
|---|---:|---:|---:|---|---:|---|---|
| 100 | 112.3, 15.0 | 112.8, 7.92 [9.17] | 146.67, 132.92 | 13.96 [13.33, 14.17] | 6.65 | **miss** | **miss** |
| 1,000 | 57.7, 5.9 | 59.4, 3.86 [6.17] | 84.09, 76.33 | 7.95 [7.65, 8.27] | 3.82 | pass | **miss** |
| 10,000 | 47.4, 5.4 | 48.2, 2.90 [3.54] | 72.99, 65.08 | 7.90 [7.72, 8.18] | 3.25 | pass | **miss** |
| 100,000 | 45.3, 5.9 | 45.7, 3.24 [3.32] | 73.73, 65.12 | 8.57 [8.27, 9.70] | 3.26 | pass | **miss** |

The quiet re-check at 7a5b0020 measured C3-k100 at N 119.4 ns and a delta of 6.04 ns [upper bound
16.04], and the amendment registered round 3 accepting that C3-k100 lacked margin. Round 3's deltas
are about 1.8 to 2.7 times the informal profiles': 7.9 to 8.6 ns at `K` 1,000 and above against 2.9
to 3.9, and 14.0 ns at `K` 100 against 6.0 to 7.9. Both arms are also slower than in the profiles: N
is 65.1 ns at `K` 10,000 and 100,000 against 45.7 to 48.2 in the profile and 45.3 to 47.4 in round
2, while C1's and C2's N at the same `K` are about 5% to 9% above round 2's. The cause of the larger
deltas and of the slower arms is not known. Round 2 measured C3-k100 at 15.0 ns, which ADR 0025 also
puts over its floor.

## Decisions

- The CPU budget is missed at C3-k100. Under the note's decision rules the miss is a defect in the
  backend, to be fixed and measured by a later round; no budget changes, and no later round is
  registered by this page.
- The 5% rule is reported and decides nothing; by it C3 misses at every judged `K`, as in round 2.
- Allocations now meet their budget in every judged configuration; C10m1's one allocation a frame
  over N stays reported, the cost of keeping the records' layout (issue #31).
- Texture indexing is not reopened (issue #30): the composed frame meets its GPU budget.
- No pipeline variant is hidden, and every hot path stays within its native-call budget.
- No mapping row of the reference changes: every stated cost class holds.
