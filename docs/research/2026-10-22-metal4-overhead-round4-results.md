# The Metal 4 backend's overhead against native code: round 4 results

**Status**: Accepted (2026-10-22)

These are the results of round 4, which [the round-4 amendment](
2026-10-22-metal4-overhead-round4.md) registered under [the pre-registration](
2026-10-18-metal4-overhead.md), judged by that note's decision rules as amended for round 3, with
CPU time per operation judged by [ADR 0025](../decisions/0025-cpu-budget-per-call-floor.md).
Rounds 1 to 3 ([round 1](2026-10-18-metal4-overhead-results.md), [round 2](
2026-10-20-metal4-overhead-round2-results.md), [round 3](
2026-10-21-metal4-overhead-round3-results.md)) stay as they were recorded; the note's Results
section summarises every round, and this page holds every row of round 4.

## What changed since round 3

Round 3 ran at 378f219a and missed one budget in one configuration, CPU per operation at C3-k100.
Round 4 changes no budget, workload, `K`, arm or rule: it keeps round 3's, ADR 0025 included, with
the 5% verdict reported beside it and C10 at `maxCount` 1 (C10m1) reported only for allocations.
Its amendment lists the eight commits on `main` since round 3 that touch the arms' code, with their
effect on each arm: six come with the D3D12 harness and leave each Metal 4 arm's native calls and
allocations the same, one makes R's `setPipeline` pick a cached pipeline without a branch on which
entry matched (the fix for C3), and one adds `bench.py seal`, which writes the evidence manifest
last. No native-call or allocation count of either arm changes.

## Measurement

Measured on 2026-10-22 from 11:59 to 20:39 CDT on an Apple M3 Max (Apple7 to Apple9, Metal 4,
timestamps at 24 MHz) under macOS 27.0.1 (26A434), Xcode 26.6, `metal` 32023.883, Apple clang
21.0.0 and `metal-cpp` c595afe, on AC power, from a clean checkout of 18ab6cf2, the tip of `main`
once the amendment's pull request had landed. `bench.py build` finished at 11:59, and the
correctness gate ran at that commit from 12:02 to 12:03, before anything was timed, and passed every
workload in Release and in Debug. Timing ran from 12:04 to 20:39, one workload batch at a time, and
`analyze` followed. All 1,260 timed processes (63 configurations of 20) started and ended at the
nominal thermal state on their first attempt: none was discarded, no configuration is reported not
run, and no process printed a line on standard error, so the round was not stopped and no scope
went untimed. Raw results, logs, the driver's output, the load logs below, `summary.json`,
`analysis.txt` and a SHA-256 manifest are kept outside the published tree, under
`rojo-rhi-evidence/t2/2026-10-22-round4/`. `bench.py seal` wrote the manifest last, and every entry
verifies, the driver's own output included.

## Load

Before the correctness gate and before each workload batch, a check waited until no build,
compiler, test or bench process and no foreign process above 30% of a core had run for 60 s, and
recorded the load; the gate and all 14 batches started after such a quiet minute. During the round
a watcher logged every 30 s any process above 30% of a core.

The owner's terminal ran through C8 and after it. The watcher first logged the terminal, Ghostty, at
13:06:20, during C8-k10000's process 0 (C8 ran from 13:05:27 to 14:59:08), and then in each of the
224 samples it logged to the end of the batch: 32% to 63% of a core until 13:30, rising to 79% to
104% in its last half hour. `lazygit` in that terminal joined it from 13:15:53, at 30% to 42% until
13:30 and 66% to 86% in the last half hour. So every C8 process from C8-k10000's process 0 on, 96 of
its 100, ran beside them; the four that ran before (process 0 at `K` 1 to 1,000) did not. In the
same batch macOS's XProtect remediators ran from 13:51:33 to 14:08:38 at 31% to 97% (33 samples,
during C8's processes 7 to 10), with `PerfPowerServices` at 108% at 13:58:05 and its signpost reader
at 93% to 95% from 14:01:36 to 14:02:06. After C8 the terminal and `lazygit` stayed busy (Ghostty
56% to 114%, `lazygit` 84% to 140%) from 14:59:09 to 20:21:24, and the check before C9 held the
round for those five hours and 25 minutes, until 20:24:10; no other batch ran under them.

Every other process the watcher logged during a batch was the desktop's own, briefly: `launchd` in
C1, a browser renderer in C4 (38%), C6 (84%) and C10 (45%), `duetexpertd` (81%) and Obsidian in C6,
`mds` in C10, and, in C8, `duetexpertd`, `trustd`, `mobileassetd`, `logd`, `launchd`, `sysmond`
(71%), a widget and a browser renderer. C2, C3, C5, C7, C9, C10m1, G1, G2 and the composed frame ran
with nothing logged. The registered process checks, the thermal discard and the stop on a line on
standard error, did not fire, so no process was discarded and the results stand as registered;
nothing is rerun, since the note allows no selective rerun.

C8 is judged with R far below N at every judged `K`, under the load above. Both arms' CPU times
differ from round 3's (N 6,724.79 ns per operation at `K` 100 against 15,062.71, R 3,542.19 against
6,537.29), and the cause is not known; R stays below N by 47% to 58% at every judged `K` (round 3:
30% to 66%). C3, whose `K` 100 is the tight configuration, ran from 12:16:45 to 12:21:28 with
nothing logged.

## Verdict

The round meets all five budgets: no judged configuration misses. Judged by the note's original 5%
rule, which round 4 reports and does not decide by, CPU would miss in three of 34, C3 at `K` 100,
10,000 and 100,000.

| Budget | Round 3 (2026-10-21) | Round 4 | The 5% rule on round 4 (reported) |
|---|---|---|---|
| CPU tax per operation, at most the larger of 5% of N and 5 ns times `c` | **missed**: 1 of 34, C3-k100, 13.96 ns [13.33, 14.17] against 10 | met: no judged configuration misses. C3-k100: R 142.08 ns, N 133.33 ns, delta 8.33 ns [7.92, 9.06] against a budget of 10.00 ns, +6.25%; C3 at `K` 1,000 to 100,000 3.48 to 3.74 ns against 10.00; C6-k100 5.83 against 10.00; C10-k100 37.08 against 45.54 | would miss 3 of 34: C3-k100 +6.25%, C3-k10000 +5.32%, C3-k100000 +5.55%. Every other judged configuration is within 5%, at most +4.90% (C3-k1000) |
| GPU frame-time tax, composed frame, at most 1% | met: −43.25 µs [−50.79, −37.59], −2.20% | met: R 1,916.72 µs, N 1,955.03 µs, −39.56 µs [−48.57, −34.45], −2.02% | - |
| Hidden pipeline variants, zero | met | met: no pipeline and no native creation after warm-up in any process of any configuration | - |
| Per-frame heap allocations, R at most N | met in every judged configuration; C10m1 reported, +1 | met in every judged configuration: R equals N on C1 to C7, C9-k1, C10, G1 and G2, and is below it on C8 (by 3 to 29,980), on C9 at `K` 10 and 100 (medians 0.5 below) and on the composed frame (157 against 174); RojoRHI's own `cpuAllocations` is 0 everywhere. C10m1, reported only: R above N by 1 at every `K` (15 against 14, 16 against 15 at `K` 1,000) | - |
| Native calls, at most N plus `c` | met: C8 13 against 19, C9 2 against 3 | met: every operation of C1 to C9 is within, with stable counts; C8 13 against a limit of 19, C9 2 against 3; C10 3 against N's 3 and C10m1 1 against N's 1, reported | - |

## Rows

**CPU and GPU time**, per configuration: CPU nanoseconds per operation and GPU microseconds per
frame, the median of process medians for R and N, and the median of the paired deltas R minus N with
its 95% bootstrap interval. The relative column is the delta over N's median. CPU is judged at `K`
of 100 and above for C1 to C10, against the budget column (ADR 0025), with the 5% verdict beside
it; GPU is judged on the composed frame only; every other cell is reported. C10m1 is reported only.
The composed frame has no CPU span. Values are those of `analysis.txt`, GPU converted from ns.

| Id | CPU R, ns/op | CPU N | Delta [95% interval] | Rel. | Budget, ns | CPU rule | 5% rule | GPU R, µs | GPU N | Delta, µs [95% interval] | Rel. | GPU rule |
|---|---:|---:|---|---:|---:|---|---|---:|---:|---|---:|---|
| C1-k1 | 6,125.00 | 5,750.00 | 333.33 [270.83, 385.42] | +5.80% | - | reported | - | 13.80 | 13.71 | 0.23 [0.02, 0.86] | +1.67% | reported |
| C1-k10 | 708.33 | 675.00 | 33.33 [29.17, 41.67] | +4.94% | - | reported | - | 13.01 | 12.65 | 0.65 [0.03, 0.88] | +5.11% | reported |
| C1-k100 | 143.54 | 137.71 | 5.31 [5.00, 6.15] | +3.86% | 15.00 | pass | pass | 16.66 | 16.62 | 0.04 [-0.02, 0.17] | +0.25% | reported |
| C1-k1000 | 77.12 | 75.42 | 1.85 [1.40, 2.74] | +2.46% | 15.00 | pass | pass | 62.81 | 63.19 | -0.73 [-1.85, 0.79] | -1.15% | reported |
| C1-k10000 | 66.76 | 65.23 | 1.72 [1.15, 2.62] | +2.64% | 15.00 | pass | pass | 156.37 | 157.54 | -1.38 [-2.39, -0.32] | -0.87% | reported |
| C1-k100000 | 66.39 | 64.58 | 1.78 [1.07, 2.22] | +2.76% | 15.00 | pass | pass | 1,190.76 | 1,169.25 | 22.28 [16.66, 25.14] | +1.91% | reported |
| C2-k1 | 6,145.83 | 5,750.00 | 395.83 [375.00, 416.67] | +6.88% | - | reported | - | 13.85 | 13.70 | 0.05 [-0.02, 0.26] | +0.38% | reported |
| C2-k10 | 716.67 | 673.96 | 41.67 [37.50, 50.00] | +6.18% | - | reported | - | 12.82 | 12.89 | 0.32 [-0.54, 0.77] | +2.51% | reported |
| C2-k100 | 143.75 | 139.58 | 4.38 [3.54, 5.42] | +3.13% | 15.00 | pass | pass | 15.92 | 15.86 | 0.05 [-0.19, 0.18] | +0.33% | reported |
| C2-k1000 | 77.39 | 76.10 | 1.16 [0.69, 1.73] | +1.52% | 15.00 | pass | pass | 61.82 | 61.70 | 0.00 [-1.93, 0.89] | +0.00% | reported |
| C2-k10000 | 67.17 | 66.59 | 0.83 [0.14, 2.46] | +1.25% | 15.00 | pass | pass | 157.58 | 159.69 | -2.20 [-4.44, -0.93] | -1.38% | reported |
| C2-k100000 | 67.43 | 65.26 | 1.59 [0.37, 2.52] | +2.43% | 15.00 | pass | pass | 1,206.97 | 1,187.52 | 20.48 [18.00, 25.00] | +1.72% | reported |
| C3-k1 | 6,250.00 | 5,791.67 | 437.50 [395.83, 489.58] | +7.55% | - | reported | - | 13.73 | 13.76 | 0.00 [-0.08, 0.08] | +0.00% | reported |
| C3-k10 | 708.33 | 660.42 | 50.00 [45.83, 54.17] | +7.57% | - | reported | - | 13.63 | 13.23 | 0.32 [0.13, 0.53] | +2.44% | reported |
| C3-k100 | 142.08 | 133.33 | 8.33 [7.92, 9.06] | +6.25% | 10.00 | pass | **miss** | 18.81 | 18.85 | -0.11 [-0.36, 0.17] | -0.61% | reported |
| C3-k1000 | 80.16 | 76.40 | 3.74 [3.55, 3.88] | +4.90% | 10.00 | pass | pass | 94.48 | 94.50 | 0.03 [-0.07, 0.20] | +0.03% | reported |
| C3-k10000 | 68.94 | 65.29 | 3.48 [3.23, 3.69] | +5.32% | 10.00 | pass | **miss** | 407.89 | 434.47 | -25.00 [-31.80, -7.91] | -5.75% | reported |
| C3-k100000 | 68.94 | 65.37 | 3.63 [3.51, 3.70] | +5.55% | 10.00 | pass | **miss** | 4,912.23 | 4,907.02 | 4.02 [0.11, 9.04] | +0.08% | reported |
| C4-k1 | 3,354.17 | 3,166.67 | 197.92 [125.00, 281.25] | +6.25% | - | reported | - | 2.60 | 2.42 | 0.22 [0.08, 0.28] | +9.05% | reported |
| C4-k10 | 441.67 | 416.67 | 25.00 [23.96, 34.38] | +6.00% | - | reported | - | 3.88 | 3.79 | 0.08 [0.04, 0.21] | +2.20% | reported |
| C4-k100 | 117.08 | 115.31 | 2.50 [0.83, 5.00] | +2.17% | 15.00 | pass | pass | 24.21 | 24.12 | 0.10 [-0.02, 0.31] | +0.43% | reported |
| C4-k1000 | 79.66 | 79.52 | 0.33 [-2.10, 1.79] | +0.42% | 15.00 | pass | pass | 242.26 | 242.25 | -0.18 [-0.94, 0.58] | -0.07% | reported |
| C4-k10000 | 70.61 | 70.76 | 0.07 [-1.64, 1.50] | +0.10% | 15.00 | pass | pass | 2,295.29 | 2,292.29 | -1.73 [-8.16, 7.94] | -0.08% | reported |
| C4-k100000 | 70.88 | 69.88 | 0.19 [-1.77, 2.22] | +0.27% | 15.00 | pass | pass | 17,338.28 | 17,220.41 | 139.33 [53.53, 183.52] | +0.81% | reported |
| C5-k1 | 916.67 | 916.67 | 0.00 [-41.67, 41.67] | +0.00% | - | reported | - | 0.00 | 0.00 | 0.00 [0.00, 0.00] | - | reported |
| C5-k10 | 95.83 | 93.75 | 0.00 [-2.08, 4.17] | +0.00% | - | reported | - | 0.00 | 0.00 | 0.00 [0.00, 0.00] | - | reported |
| C5-k100 | 14.17 | 14.58 | -0.21 [-1.25, 0.62] | -1.43% | 5.00 | pass | pass | 0.00 | 0.00 | 0.00 [0.00, 0.00] | - | reported |
| C5-k1000 | 5.71 | 6.19 | -0.50 [-1.08, 0.12] | -8.08% | 5.00 | pass | pass | 0.00 | 0.00 | 0.00 [0.00, 0.00] | - | reported |
| C5-k10000 | 5.10 | 5.07 | 0.03 [-0.01, 0.04] | +0.53% | 5.00 | pass | pass | 0.00 | 0.00 | 0.00 [0.00, 0.00] | - | reported |
| C5-k100000 | 4.39 | 4.34 | 0.06 [0.06, 0.09] | +1.47% | 5.00 | pass | pass | 0.00 | 0.00 | 0.00 [0.00, 0.00] | - | reported |
| C6-k1 | 4,125.00 | 3,916.67 | 229.17 [104.17, 291.67] | +5.85% | - | reported | - | 4.30 | 4.25 | 0.05 [-0.08, 0.12] | +1.23% | reported |
| C6-k10 | 501.04 | 480.21 | 20.83 [16.67, 25.00] | +4.34% | - | reported | - | 18.70 | 18.86 | -0.15 [-0.29, 0.04] | -0.77% | reported |
| C6-k100 | 132.50 | 126.67 | 5.83 [4.79, 6.25] | +4.61% | 10.00 | pass | pass | 166.47 | 167.14 | -0.30 [-0.84, 1.66] | -0.18% | reported |
| C6-k1000 | 113.23 | 110.91 | 2.04 [1.74, 2.44] | +1.84% | 10.00 | pass | pass | 1,573.03 | 1,574.14 | -2.11 [-4.45, 5.06] | -0.13% | reported |
| C6-k10000 | 77.76 | 74.97 | 2.79 [2.48, 3.00] | +3.72% | 10.00 | pass | pass | 15,653.39 | 15,653.13 | -1.80 [-19.64, 14.70] | -0.01% | reported |
| C6-k100000 | 80.96 | 77.78 | 3.08 [2.92, 3.40] | +3.96% | 10.00 | pass | pass | 124,773.74 | 125,906.04 | -958.61 [-1,545.22, -240.58] | -0.76% | reported |
| C7-k1 | 3,666.67 | 3,541.67 | 145.83 [125.00, 187.50] | +4.12% | - | reported | - | 2.87 | 2.69 | 0.13 [0.00, 0.24] | +4.65% | reported |
| C7-k10 | 490.62 | 470.83 | 16.67 [12.50, 20.83] | +3.54% | - | reported | - | 4.21 | 4.13 | 0.10 [0.04, 0.40] | +2.52% | reported |
| C7-k100 | 147.50 | 145.42 | 1.67 [1.25, 2.92] | +1.15% | 7.27 | pass | pass | 24.69 | 24.56 | 0.17 [0.08, 0.23] | +0.68% | reported |
| C7-k1000 | 101.92 | 100.64 | 1.24 [0.73, 1.51] | +1.23% | 5.03 | pass | pass | 248.12 | 248.13 | 0.06 [-0.11, 0.53] | +0.03% | reported |
| C7-k10000 | 94.28 | 93.78 | 0.51 [0.06, 0.87] | +0.55% | 5.00 | pass | pass | 2,360.03 | 2,360.31 | -1.54 [-11.71, 4.70] | -0.07% | reported |
| C7-k100000 | 95.02 | 94.28 | 0.86 [0.22, 1.26] | +0.91% | 5.00 | pass | pass | 16,603.04 | 16,582.08 | 43.00 [0.20, 77.08] | +0.26% | reported |
| C8-k1 | 6,583.33 | 9,729.17 | -3,208.33 [-3,395.83, -2,885.42] | -32.98% | - | reported | - | 13.57 | 32.22 | -18.36 [-19.20, -17.55] | -57.00% | reported |
| C8-k10 | 3,692.71 | 7,011.46 | -3,221.88 [-3,781.25, -2,877.08] | -45.95% | - | reported | - | 305.02 | 668.00 | -362.61 [-440.16, -331.93] | -54.28% | reported |
| C8-k100 | 3,542.19 | 6,724.79 | -3,182.60 [-3,698.85, -2,872.71] | -47.33% | 336.24 | pass | pass | 3,809.40 | 7,823.28 | -3,909.41 [-4,098.95, -3,819.43] | -49.97% | reported |
| C8-k1000 | 3,613.50 | 6,938.18 | -3,322.20 [-3,722.30, -3,094.89] | -47.88% | 346.91 | pass | pass | 42,298.13 | 81,693.49 | -35,954.06 [-40,697.26, -32,878.34] | -44.01% | reported |
| C8-k10000 | 3,260.74 | 7,710.14 | -4,433.52 [-4,510.98, -4,404.58] | -57.50% | 385.51 | pass | pass | 395,450.61 | 673,150.93 | -278,765.51 [-281,052.45, -274,776.22] | -41.41% | reported |
| C9-k1 | 2,083.33 | 2,083.33 | -62.50 [-104.17, 52.08] | -3.00% | - | reported | - | 2.73 | 2.63 | 0.08 [0.04, 0.13] | +3.17% | reported |
| C9-k10 | 900.00 | 903.12 | 0.00 [-15.62, 9.37] | +0.00% | - | reported | - | 260.43 | 259.79 | 4.12 [-4.22, 18.19] | +1.59% | reported |
| C9-k100 | 650.73 | 640.42 | 13.85 [-6.77, 35.42] | +2.16% | 32.02 | pass | pass | 3,186.25 | 3,117.52 | 32.06 [-0.55, 103.49] | +1.03% | reported |
| C10-k1 | 9,166.67 | 8,458.33 | 708.33 [552.08, 812.50] | +8.37% | - | reported | - | 76.44 | 75.51 | 0.10 [-1.44, 4.63] | +0.14% | reported |
| C10m1-k1 | 7,791.67 | 5,541.67 | 2,239.58 [2,187.50, 2,291.67] | +40.41% | - | reported | - | 32.34 | 12.81 | 19.59 [19.39, 19.77] | +152.93% | reported |
| C10-k10 | 4,239.58 | 3,878.12 | 332.29 [289.58, 458.33] | +8.57% | - | reported | - | 270.81 | 270.68 | 0.24 [-1.04, 1.91] | +0.09% | reported |
| C10m1-k10 | 866.67 | 620.83 | 245.83 [241.67, 250.00] | +39.60% | - | reported | - | 33.04 | 11.74 | 21.42 [21.10, 21.60] | +182.43% | reported |
| C10-k100 | 949.17 | 910.73 | 37.08 [35.62, 40.00] | +4.07% | 45.54 | pass | pass | 1,664.68 | 1,666.32 | -8.31 [-13.81, -1.99] | -0.50% | reported |
| C10m1-k100 | 148.85 | 111.88 | 37.08 [36.67, 37.92] | +33.15% | - | reported | - | 46.28 | 14.50 | 31.61 [30.60, 32.02] | +218.03% | reported |
| C10-k1000 | 597.95 | 611.82 | -12.45 [-29.49, 1.21] | -2.03% | 30.59 | pass | pass | 16,955.05 | 16,845.44 | 103.78 [-9.15, 149.98] | +0.62% | reported |
| C10m1-k1000 | 71.88 | 59.95 | 12.19 [11.79, 12.49] | +20.33% | - | reported | - | 132.40 | 59.82 | 72.79 [71.85, 73.36] | +121.68% | reported |
| G1-uniform | 3,250.00 | 3,000.00 | 208.33 [166.67, 250.00] | +6.94% | - | reported | - | 61.61 | 51.00 | 10.67 [10.17, 12.66] | +20.92% | reported |
| G1-divergent | 8,916.67 | 8,250.00 | 645.83 [583.33, 781.25] | +7.83% | - | reported | - | 430.00 | 425.75 | 4.29 [4.07, 4.82] | +1.01% | reported |
| G2-uniform | 3,375.00 | 3,041.67 | 312.50 [291.67, 333.33] | +10.27% | - | reported | - | 62.31 | 62.15 | 0.13 [0.05, 0.25] | +0.20% | reported |
| G2-divergent | 9,291.67 | 8,500.00 | 791.67 [656.25, 906.25] | +9.31% | - | reported | - | 1,541.89 | 1,541.75 | 0.20 [0.14, 0.31] | +0.01% | reported |
| frame | - | - | - | - | - | reported | - | 1,916.72 | 1,955.03 | -39.56 [-48.57, -34.45] | -2.02% | pass |

G2's arm F, for the record: 57.23 µs against N's 62.15 µs uniform (−4.92 µs [−5.03, −4.85],
−7.91%) and 1,540.36 against 1,541.75 µs divergent (−1.43 µs [−1.51, −1.22], −0.09%).

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
RojoRHI's own `cpuAllocations` for R. C10m1's rows are reported only.

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
| C8-k10000 | 40,030 | 70,010 | -29,980 [-29,980, -29,980] | 0 | pass |
| C9-k1 | 18 | 18 | 0 [0, 0] | 0 | pass |
| C9-k10 | 154 | 154.5 | -0.5 [-1, 0] | 0 | pass |
| C9-k100 | 1,514.5 | 1,515 | -0.5 [-1, 0] | 0 | pass |
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
1). The departure from the procedure is the terminal's load during C8, above.

## C3 across rounds

C3 binds one of two pipelines, alternating, then draws: `c` 2, so its budget is 10 ns at every `K`,
against 5% of N's 3.26 to 6.67 ns in round 4. Per operation, in nanoseconds; the re-check is the
amendment's, informal and not judged, at 229cc278 with the round's own method:

| `K` | Round 2: N, delta | Round 3: N, delta [95% interval] | Re-check: N, delta [95% interval] | Round 4: R, N | Round 4: delta [95% interval] | 5% of N | ADR 0025 | 5% rule |
|---|---:|---:|---:|---:|---|---:|---|---|
| 100 | 112.3, 15.0 | 132.92, 13.96 [13.33, 14.17] | 133.33, 8.54 [7.08, 9.69] | 142.08, 133.33 | 8.33 [7.92, 9.06] | 6.67 | pass | **miss** |
| 1,000 | 57.7, 5.9 | 76.33, 7.95 [7.65, 8.27] | 76.53, 3.33 [3.09, 3.75] | 80.16, 76.40 | 3.74 [3.55, 3.88] | 3.82 | pass | pass |
| 10,000 | 47.4, 5.4 | 65.08, 7.90 [7.72, 8.18] | 65.26, 3.11 [3.06, 3.30] | 68.94, 65.29 | 3.48 [3.23, 3.69] | 3.26 | pass | **miss** |
| 100,000 | 45.3, 5.9 | 65.12, 8.57 [8.27, 9.70] | 65.24, 3.38 [3.17, 3.52] | 68.94, 65.37 | 3.63 [3.51, 3.70] | 3.27 | pass | **miss** |

Round 4's deltas are within 0.41 ns of the re-check's. At `K` 1,000 and above they are less than
half of round 3's (3.48 to 3.74 ns against 7.90 to 8.57), and at `K` 100 about 0.6 of it (8.33
against 13.96).
N's times are round 3's, which the amendment attributes to the shader change of pull request #100.
C3-k100 passes without the margin the owner asks for before registering: its median is 0.83 of the
budget and its upper bound 0.94 ns under it. Its paired deltas range from 2.71 to 12.08 ns across
its 20 processes, four of them above 10 ns; the judged value is their median.

## Decisions

- No budget is missed. The backend's overhead exit is met: every budget of ADR 0014, as ADR 0025
  amends its CPU budget, is judged met in round 4.
- The 5% rule is reported and decides nothing; by it C3 misses at `K` 100 (+6.25%), 10,000 (+5.32%)
  and 100,000 (+5.55%), by 0.21 to 1.67 ns per operation over 5% of N.
- C10m1's one allocation a frame over N stays reported, the cost of keeping the records' layout
  (issue #31).
- Texture indexing is not reopened (issue #30): the composed frame meets its GPU budget.
- No pipeline variant is hidden, and every hot path stays within its native-call budget.
- No mapping row of the reference changes: every stated cost class holds.
