# The Metal 4 backend's overhead against native code: round 2 results

**Status**: Accepted (2026-10-20)

These are the results of round 2, which [the round-2 amendment](
2026-10-20-metal4-overhead-round2.md) registered under [the pre-registration](
2026-10-18-metal4-overhead.md), judged by that note's decision rules as amended. Round 1's
[results](2026-10-18-metal4-overhead-results.md) stay as they were recorded; the note's Results
section summarises both rounds, and this page holds every row.

## Measurement

Measured on 2026-10-20 from 04:26 to 08:31 CDT on an Apple M3 Max (Apple7 to Apple9, Metal 4,
timestamps at 24 MHz) under macOS 27.0.1 (26A434), Xcode 26.6, `metal` 32023.883, Apple clang 21.0.0
and `metal-cpp` c595afe, on AC power, from a clean checkout of 0a0b53f, the commit that merged the
amendment. `bench.py build` ran at 04:26, and the correctness gate ran at that commit before
anything was timed and passed every workload in Release and in Debug. Timing ran from 04:30 to
08:31, one workload batch at a time, and `analyze` followed. All 1,260 timed processes (63
configurations of 20) started and ended at the nominal thermal state on their first attempt: none
was discarded, no configuration is reported not run, and no process printed a line on standard
error, so the round was not stopped and no scope went untimed. Raw results, logs, the driver's
output, the load logs below, `summary.json`, `analysis.txt` and a SHA-256 manifest are kept outside
the published tree, under `rojo-rhi-evidence/t2/2026-10-20-round2/`.

## Load

Before each workload batch a check waited until no build, compiler, test or bench process and no
foreign process above 30% of a core had run for 60 s, and recorded the load; all 14 batches started
after such a quiet minute. During timing a watcher logged every 30 s any such process. The amendment
asks that nothing else be built, tested or timed on the machine; another session on it, working on
the D3D12 backend in a separate worktree, did so in short windows. Against each batch's timing
window, from its first to its last result file:

| Time | Activity of the other session | Batch at that time |
|---|---|---|
| 04:31:56 to 04:34:27 | `clangd` indexing its worktree, 3% to 87% of one core | C1 (04:30:07 to 04:33:06), from a start between 04:31:26 and 04:31:56, so at least processes 12 to 19 of each configuration; then the check before C2 |
| 04:58:33 | a shell running `git commit` and `git push`, one sample | C6 (04:55:33 to 05:21:31), during one process of C6-k100000 |
| 05:32:12 to 05:35:13 | `clangd`, then `xmake build RojoRHITests` with `slangc` | none: after C7 ended (05:31:26); the check held C8 back to 05:38:43 |
| 08:16:09 to 08:23:42 | a shell polling CI status, at no measurable CPU, then `git commit` and `git push` | C10m1 (08:15:39 to 08:16:11), from a start after 08:15:39, then the check before G1, which started at 08:27:13 |

The other lines of the watch are the desktop's own daemons and applications (Spotlight, a browser
renderer, media analysis) briefly above 30% of a core, across every batch. The registered process
checks, the thermal discard and the stop on a line on standard error, did not fire, so no process
was discarded and the results stand as registered; nothing is rerun, since the note allows no
selective rerun. The overlap is a limitation of this round: the other session has since confirmed
that it also ran a local Null Debug build and its tests during and around C1-k100000, more than the
watcher's 30-second samples show, and it now pauses its builds while a timing process runs. The
configurations whose timing windows overlap the other session are C1's six, C6's six and C10m1's
four. C1-k100000's narrow miss, +5.11% against the 5% budget, is possibly affected. Its eight loaded
processes show no larger delta than the twelve before them, a median paired delta of +4.3% against
+5.4%, and its paired deltas alternate with the arm order the note registers: +2.0% to +4.7% when R
runs first and +5.7% to +9.3% when N does (reported only).

## Verdict

The round misses two of the five budgets, CPU per operation and allocations, where round 1 missed
three; the composed frame now meets its GPU budget.

| Budget | Round 1 (2026-10-19) | Round 2 |
|---|---|---|
| CPU tax per operation, at most 5% | **missed**: 33 of 34 judged configurations | **missed**: 15 of 34. Misses: C1-k100 (+8.0%) and C1-k100000 (+5.1%); C2 at every `K` from 100 (+6.6% to +9.0%); C3 at every `K` from 100 (+10.3% to +13.4%); C5-k10000 (+5.4%); C6 at every `K` from 100 (+5.8% to +11.5%). Passes: C1-k1000 and C1-k10000 (+3.0%, +4.2%); C4 at every `K` (at most +4.2%); C5-k100, C5-k1000 and C5-k100000 (at most +4.5%); C7 at every `K` (at most +2.7%); C8 at every `K` (R below N by 29% to 71%); C9-k100 (+1.8%, interval includes zero); C10-k100 and C10-k1000 (+1.6%, +2.7%) |
| GPU frame-time tax, composed frame, at most 1% | **missed**: +85.6 µs [82.7, 89.6], +4.37% | met: R 1,902.6 µs, N 1,951.0 µs, −46.9 µs [−59.0, −38.5], −2.40% |
| Hidden pipeline variants, zero | met | met: no pipeline and no native creation after warm-up in any process of any configuration |
| Per-frame heap allocations, R at most N | **missed**: R above N in 61 of 63 configurations | **missed**: R above N in 57 of 63, by 2 to 7 a frame (C1 to C3 +5, C4 +3, C5 +2, C6 +4, C7 +2, C9 +2, C10 +5, C10m1 +6 or +7, G1 and G2 +3). Passes: C8 at every `K` (R below N by 1 to 29,979) and the composed frame (157 against 172); RojoRHI's own `cpuAllocations` is 0 everywhere |
| Native calls, at most N plus `c` | met: C8 14 against a limit of 19, C9 3 against 3 | met: every operation of C1 to C9 is within, with stable counts; C8 13 against a limit of 19, C9 2 against 3 |

## Rows

**CPU and GPU time**, per configuration: CPU nanoseconds per operation and GPU microseconds per
frame, the median of process medians for R and N, and the median of the paired deltas R minus N with
its 95% bootstrap interval. The relative column is the delta over N's median. CPU is judged at `K`
of 100 and above for C1 to C10, GPU on the composed frame only; every other cell is reported. The
composed frame has no CPU span. C5's CPU cells carry two decimals, since its whole operation is
about 5 ns.

| Id | CPU R, ns/op | CPU N | Delta [95% interval] | Rel. | CPU rule | GPU R, µs | GPU N | Delta, µs [95% interval] | Rel. | GPU rule |
|---|---:|---:|---|---:|---|---:|---:|---|---:|---|
| C1-k1 | 6,937.5 | 5,750.0 | 916.7 [750.0, 1,145.8] | +15.9% | reported | 10.1 | 11.2 | -0.8 [-0.9, -0.7] | -7.5% | reported |
| C1-k10 | 750.0 | 662.5 | 102.1 [89.6, 111.5] | +15.4% | reported | 10.2 | 11.0 | -0.8 [-0.9, -0.7] | -7.4% | reported |
| C1-k100 | 138.1 | 128.3 | 10.3 [7.4, 12.3] | +8.0% | **miss** | 11.3 | 11.4 | 0.0 [-0.1, 0.1] | -0.1% | reported |
| C1-k1000 | 74.1 | 71.4 | 2.2 [1.3, 3.5] | +3.0% | pass | 44.0 | 44.1 | 0.0 [-0.1, 0.0] | -0.0% | reported |
| C1-k10000 | 66.1 | 62.5 | 2.6 [1.9, 3.5] | +4.2% | pass | 133.3 | 138.6 | -1.0 [-4.2, 0.7] | -0.7% | reported |
| C1-k100000 | 65.4 | 61.8 | 3.2 [1.8, 3.8] | +5.1% | **miss** | 1,047.7 | 1,048.3 | 3.4 [0.5, 23.4] | +0.3% | reported |
| C2-k1 | 6,541.7 | 5,750.0 | 833.3 [781.3, 895.8] | +14.5% | reported | 13.6 | 13.8 | -0.4 [-0.7, 0.1] | -2.6% | reported |
| C2-k10 | 754.2 | 654.2 | 97.9 [95.8, 104.2] | +15.0% | reported | 13.5 | 13.6 | -0.5 [-0.6, 0.2] | -3.6% | reported |
| C2-k100 | 141.7 | 129.8 | 11.7 [10.2, 12.7] | +9.0% | **miss** | 15.1 | 15.1 | -0.1 [-0.2, 0.2] | -0.5% | reported |
| C2-k1000 | 76.5 | 71.7 | 4.7 [3.4, 5.5] | +6.6% | **miss** | 61.2 | 62.0 | -0.2 [-1.1, 0.0] | -0.3% | reported |
| C2-k10000 | 66.0 | 61.8 | 4.9 [3.6, 5.5] | +8.0% | **miss** | 145.9 | 146.5 | -1.2 [-2.2, -0.4] | -0.8% | reported |
| C2-k100000 | 65.8 | 61.4 | 4.5 [3.1, 5.5] | +7.4% | **miss** | 1,116.6 | 1,096.2 | 21.6 [17.2, 24.4] | +2.0% | reported |
| C3-k1 | 6,583.3 | 5,708.3 | 833.3 [833.3, 916.7] | +14.6% | reported | 13.7 | 14.4 | -0.7 [-0.8, -0.6] | -5.1% | reported |
| C3-k10 | 720.8 | 629.2 | 91.7 [85.4, 95.8] | +14.6% | reported | 13.2 | 14.3 | -1.1 [-1.3, -0.9] | -7.6% | reported |
| C3-k100 | 126.5 | 112.3 | 15.0 [14.4, 16.0] | +13.4% | **miss** | 18.3 | 18.2 | 0.2 [-0.4, 0.3] | +1.0% | reported |
| C3-k1000 | 63.9 | 57.7 | 5.9 [5.6, 6.4] | +10.3% | **miss** | 92.5 | 92.5 | 0.0 [-0.1, 0.3] | +0.0% | reported |
| C3-k10000 | 52.7 | 47.4 | 5.4 [5.1, 5.7] | +11.5% | **miss** | 394.2 | 410.8 | -8.7 [-17.7, -0.8] | -2.1% | reported |
| C3-k100000 | 51.2 | 45.3 | 5.9 [5.8, 6.0] | +13.1% | **miss** | 4,925.0 | 4,920.4 | 3.9 [1.8, 8.8] | +0.1% | reported |
| C4-k1 | 3,625.0 | 3,000.0 | 625.0 [583.3, 666.7] | +20.8% | reported | 2.6 | 2.5 | 0.1 [0.1, 0.2] | +5.5% | reported |
| C4-k10 | 468.8 | 400.0 | 70.8 [66.7, 71.9] | +17.7% | reported | 3.9 | 3.8 | 0.1 [0.0, 0.2] | +2.2% | reported |
| C4-k100 | 118.8 | 114.6 | 4.8 [2.1, 5.5] | +4.2% | pass | 24.2 | 24.3 | 0.0 [-0.1, 0.0] | -0.1% | reported |
| C4-k1000 | 79.1 | 79.6 | -0.9 [-1.9, 1.8] | -1.2% | pass | 242.4 | 242.4 | -0.1 [-0.3, 0.3] | -0.0% | reported |
| C4-k10000 | 70.6 | 71.1 | -0.5 [-2.6, 2.1] | -0.7% | pass | 2,296.9 | 2,298.4 | -3.6 [-7.6, 2.5] | -0.2% | reported |
| C4-k100000 | 70.3 | 70.4 | -0.3 [-2.4, 1.7] | -0.4% | pass | 17,220.4 | 17,273.1 | -34.0 [-171.9, 71.2] | -0.2% | reported |
| C5-k1 | 958.33 | 947.92 | 20.83 [-62.50, 83.33] | +2.2% | reported | 0.0 | 0.0 | 0.0 [0.0, 0.0] | - | reported |
| C5-k10 | 101.04 | 106.25 | -1.04 [-10.42, 12.50] | -1.0% | reported | 0.0 | 0.0 | 0.0 [0.0, 0.0] | - | reported |
| C5-k100 | 14.69 | 14.38 | 0.21 [-1.25, 1.67] | +1.4% | pass | 0.0 | 0.0 | 0.0 [0.0, 0.0] | - | reported |
| C5-k1000 | 6.04 | 6.04 | -0.02 [-0.65, 0.46] | -0.3% | pass | 0.0 | 0.0 | 0.0 [0.0, 0.0] | - | reported |
| C5-k10000 | 5.33 | 5.05 | 0.27 [0.02, 0.30] | +5.4% | **miss** | 0.0 | 0.0 | 0.0 [0.0, 0.0] | - | reported |
| C5-k100000 | 4.56 | 4.37 | 0.20 [0.18, 0.22] | +4.5% | pass | 0.0 | 0.0 | 0.0 [0.0, 0.0] | - | reported |
| C6-k1 | 4,416.7 | 3,750.0 | 666.7 [604.2, 708.3] | +17.8% | reported | 4.1 | 4.2 | -0.1 [-0.2, 0.1] | -3.4% | reported |
| C6-k10 | 537.5 | 470.8 | 63.5 [62.5, 68.8] | +13.5% | reported | 19.0 | 19.0 | 0.0 [-0.4, 0.1] | -0.0% | reported |
| C6-k100 | 141.5 | 126.8 | 14.6 [14.2, 15.4] | +11.5% | **miss** | 165.6 | 166.8 | -1.3 [-2.1, -0.8] | -0.8% | reported |
| C6-k1000 | 117.5 | 111.1 | 6.4 [5.1, 7.2] | +5.8% | **miss** | 1,570.4 | 1,570.4 | 0.8 [-3.6, 9.9] | +0.1% | reported |
| C6-k10000 | 81.5 | 75.1 | 6.4 [6.1, 6.8] | +8.5% | **miss** | 15,563.9 | 15,553.0 | 8.1 [-22.1, 27.2] | +0.1% | reported |
| C6-k100000 | 84.7 | 77.7 | 7.0 [6.8, 7.4] | +9.1% | **miss** | 125,349.3 | 126,750.2 | -1,861.6 [-2,572.6, -1,334.2] | -1.5% | reported |
| C7-k1 | 3,791.7 | 3,364.6 | 447.9 [375.0, 500.0] | +13.3% | reported | 2.8 | 2.7 | 0.1 [0.0, 0.3] | +3.5% | reported |
| C7-k10 | 504.2 | 456.2 | 39.6 [37.5, 51.0] | +8.7% | reported | 4.2 | 4.1 | 0.0 [0.0, 0.1] | +1.0% | reported |
| C7-k100 | 148.8 | 145.0 | 4.0 [2.9, 5.2] | +2.7% | pass | 24.7 | 24.7 | 0.0 [0.0, 0.2] | +0.1% | reported |
| C7-k1000 | 101.6 | 101.1 | 0.4 [0.0, 1.4] | +0.4% | pass | 248.5 | 248.2 | 0.4 [-0.2, 0.8] | +0.2% | reported |
| C7-k10000 | 94.3 | 94.1 | 0.2 [-0.2, 0.9] | +0.2% | pass | 2,380.0 | 2,379.9 | -3.1 [-9.5, 3.8] | -0.1% | reported |
| C7-k100000 | 94.7 | 94.0 | 0.7 [0.6, 0.9] | +0.7% | pass | 17,749.6 | 17,741.2 | 0.6 [-64.9, 60.2] | +0.0% | reported |
| C8-k1 | 6,500.0 | 9,208.3 | -2,666.7 [-2,791.7, -2,593.8] | -29.0% | reported | 13.8 | 32.7 | -19.2 [-19.5, -18.3] | -58.8% | reported |
| C8-k10 | 6,465.6 | 8,525.0 | -2,090.6 [-2,185.4, -1,988.5] | -24.5% | reported | 316.5 | 688.0 | -369.2 [-376.1, -366.1] | -53.7% | reported |
| C8-k100 | 5,520.0 | 13,791.6 | -9,731.4 [-10,645.8, -6,412.5] | -70.6% | pass | 4,300.1 | 7,944.4 | -3,440.4 [-3,696.9, -3,358.4] | -43.3% | reported |
| C8-k1000 | 8,675.9 | 14,030.3 | -6,214.3 [-6,677.0, -5,220.2] | -44.3% | pass | 44,210.2 | 87,631.6 | -40,813.4 [-43,509.7, -33,303.3] | -46.6% | reported |
| C8-k10000 | 5,551.9 | 7,976.9 | -2,358.3 [-2,446.5, -2,246.6] | -29.6% | pass | 428,492.4 | 730,624.6 | -303,293.9 [-305,066.9, -300,807.3] | -41.5% | reported |
| C9-k1 | 1,916.7 | 2,125.0 | -177.1 [-229.2, -125.0] | -8.3% | reported | 3.1 | 2.6 | 0.4 [0.2, 0.6] | +15.1% | reported |
| C9-k10 | 904.2 | 894.8 | 14.6 [-1.0, 20.8] | +1.6% | reported | 266.7 | 257.8 | 8.9 [6.9, 11.6] | +3.4% | reported |
| C9-k100 | 652.7 | 642.1 | 11.7 [-17.5, 38.8] | +1.8% | pass | 3,255.7 | 3,220.7 | 31.4 [21.6, 40.3] | +1.0% | reported |
| C10-k1 | 9,520.8 | 8,416.7 | 1,125.0 [1,062.5, 1,177.1] | +13.4% | reported | 76.1 | 75.4 | 1.6 [0.3, 3.3] | +2.1% | reported |
| C10m1-k1 | 8,250.0 | 5,562.5 | 2,645.8 [2,583.3, 2,750.0] | +47.6% | reported | 37.8 | 13.7 | 24.1 [24.0, 24.3] | +176.5% | reported |
| C10-k10 | 2,004.2 | 3,845.8 | -422.9 [-2,125.0, 468.8] | -11.0% | reported | 272.8 | 273.4 | -1.0 [-1.4, 0.5] | -0.4% | reported |
| C10m1-k10 | 1,004.2 | 620.8 | 383.3 [379.2, 387.5] | +61.7% | reported | 38.9 | 11.9 | 27.1 [26.9, 27.4] | +228.2% | reported |
| C10-k100 | 950.2 | 938.0 | 14.8 [3.8, 17.4] | +1.6% | pass | 1,686.0 | 1,689.8 | -3.1 [-12.2, 6.0] | -0.2% | reported |
| C10m1-k100 | 241.6 | 111.7 | 130.0 [129.0, 130.4] | +116.4% | reported | 59.8 | 14.7 | 45.3 [40.1, 66.3] | +306.8% | reported |
| C10-k1000 | 587.5 | 570.7 | 15.4 [11.4, 24.7] | +2.7% | pass | 17,134.4 | 17,056.7 | 73.6 [43.4, 119.9] | +0.4% | reported |
| C10m1-k1000 | 160.6 | 60.0 | 100.2 [99.5, 101.3] | +167.0% | reported | 360.5 | 60.0 | 300.4 [299.4, 303.8] | +500.7% | reported |
| G1-uniform | 3,520.8 | 2,958.3 | 583.3 [520.8, 583.3] | +19.7% | reported | 65.0 | 51.1 | 13.9 [13.5, 14.2] | +27.2% | reported |
| G1-divergent | 9,541.7 | 8,104.2 | 1,447.9 [1,354.2, 1,541.7] | +17.9% | reported | 430.8 | 425.9 | 5.0 [4.7, 5.1] | +1.2% | reported |
| G2-uniform | 3,583.3 | 3,000.0 | 604.2 [552.1, 625.0] | +20.1% | reported | 62.2 | 62.0 | 0.2 [0.2, 0.3] | +0.4% | reported |
| G2-divergent | 10,145.8 | 8,354.2 | 1,833.3 [1,562.5, 3,302.1] | +21.9% | reported | 1,542.0 | 1,541.6 | 0.3 [0.2, 0.4] | +0.0% | reported |
| frame | - | - | - | - | reported | 1,902.6 | 1,951.0 | -46.9 [-59.0, -38.5] | -2.4% | pass |

G2's arm F, for the record: 57.3 µs against N's 62.0 µs uniform (−4.8 µs [−4.8, −4.7], −7.7%) and
1,540.5 against 1,541.6 µs divergent (−1.1 µs [−1.3, −1.1], −0.1%).

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
| C10 at `maxCount` 1 | 3 | 1 | - | - | reported |

**Allocations per frame** through the default malloc zone, the median of process medians, and
RojoRHI's own `cpuAllocations` for R.

| Id | Allocations R | N | Delta [95% interval] | R `cpuAllocations` | Rule |
|---|---:|---:|---|---:|---|
| C1-k1 | 17 | 12 | +5 [+5, +5] | 0 | **miss** |
| C1-k10 | 17 | 12 | +5 [+5, +5] | 0 | **miss** |
| C1-k100 | 17 | 12 | +5 [+5, +5] | 0 | **miss** |
| C1-k1000 | 18 | 13 | +5 [+5, +5] | 0 | **miss** |
| C1-k10000 | 20 | 15 | +5 [+5, +5] | 0 | **miss** |
| C1-k100000 | 25 | 20 | +5 [+5, +5] | 0 | **miss** |
| C2-k1 | 17 | 12 | +5 [+5, +5] | 0 | **miss** |
| C2-k10 | 17 | 12 | +5 [+5, +5] | 0 | **miss** |
| C2-k100 | 17 | 12 | +5 [+5, +5] | 0 | **miss** |
| C2-k1000 | 18 | 13 | +5 [+5, +5] | 0 | **miss** |
| C2-k10000 | 20 | 15 | +5 [+5, +5] | 0 | **miss** |
| C2-k100000 | 25 | 20 | +5 [+5, +5] | 0 | **miss** |
| C3-k1 | 18 | 13 | +5 [+5, +5] | 0 | **miss** |
| C3-k10 | 18 | 13 | +5 [+5, +5] | 0 | **miss** |
| C3-k100 | 18 | 13 | +5 [+5, +5] | 0 | **miss** |
| C3-k1000 | 19 | 14 | +5 [+5, +5] | 0 | **miss** |
| C3-k10000 | 20 | 15 | +5 [+5, +5] | 0 | **miss** |
| C3-k100000 | 23 | 18 | +5 [+5, +5] | 0 | **miss** |
| C4-k1 | 14 | 11 | +3 [+3, +3] | 0 | **miss** |
| C4-k10 | 14 | 11 | +3 [+3, +3] | 0 | **miss** |
| C4-k100 | 14 | 11 | +3 [+3, +3] | 0 | **miss** |
| C4-k1000 | 15 | 12 | +3 [+3, +3] | 0 | **miss** |
| C4-k10000 | 17 | 14 | +3 [+3, +3] | 0 | **miss** |
| C4-k100000 | 22 | 19 | +3 [+3, +3] | 0 | **miss** |
| C5-k1 | 12 | 10 | +2 [+2, +2] | 0 | **miss** |
| C5-k10 | 12 | 10 | +2 [+2, +2] | 0 | **miss** |
| C5-k100 | 12 | 10 | +2 [+2, +2] | 0 | **miss** |
| C5-k1000 | 12 | 10 | +2 [+2, +2] | 0 | **miss** |
| C5-k10000 | 12 | 10 | +2 [+2, +2] | 0 | **miss** |
| C5-k100000 | 12 | 10 | +2 [+2, +2] | 0 | **miss** |
| C6-k1 | 18 | 14 | +4 [+4, +4] | 0 | **miss** |
| C6-k10 | 18 | 14 | +4 [+4, +4] | 0 | **miss** |
| C6-k100 | 18 | 14 | +4 [+4, +4] | 0 | **miss** |
| C6-k1000 | 19 | 15 | +4 [+4, +4] | 0 | **miss** |
| C6-k10000 | 21 | 17 | +4 [+4, +4] | 0 | **miss** |
| C6-k100000 | 26 | 22 | +4 [+4, +4] | 0 | **miss** |
| C7-k1 | 16 | 14 | +2 [+2, +2] | 0 | **miss** |
| C7-k10 | 16 | 14 | +2 [+2, +2] | 0 | **miss** |
| C7-k100 | 16 | 14 | +2 [+2, +2] | 0 | **miss** |
| C7-k1000 | 17 | 15 | +2 [+2, +2] | 0 | **miss** |
| C7-k10000 | 20 | 18 | +2 [+2, +2] | 0 | **miss** |
| C7-k100000 | 24 | 22 | +2 [+2, +2] | 0 | **miss** |
| C8-k1 | 17 | 18 | -1 [-1, -1] | 0 | pass |
| C8-k10 | 54 | 73 | -19 [-19, -19] | 0 | pass |
| C8-k100 | 415 | 652 | -237 [-237, -237] | 0 | pass |
| C8-k1000 | 4020 | 6963 | -2943 [-2943, -2943] | 0 | pass |
| C8-k10000 | 40029 | 70008 | -29979 [-29979, -29978] | 0 | pass |
| C9-k1 | 18 | 16 | +2 [+2, +2] | 0 | **miss** |
| C9-k10 | 154 | 152 | +2 [+2, +2] | 0 | **miss** |
| C9-k100 | 1514 | 1512 | +2 [+2, +2] | 0 | **miss** |
| C10-k1 | 19 | 14 | +5 [+5, +5] | 0 | **miss** |
| C10m1-k1 | 18 | 12 | +6 [+6, +6] | 0 | **miss** |
| C10-k10 | 19 | 14 | +5 [+5, +5] | 0 | **miss** |
| C10m1-k10 | 18 | 12 | +6 [+6, +6] | 0 | **miss** |
| C10-k100 | 19 | 14 | +5 [+5, +5] | 0 | **miss** |
| C10m1-k100 | 19 | 12 | +7 [+7, +7] | 0 | **miss** |
| C10-k1000 | 22 | 17 | +5 [+5, +5] | 0 | **miss** |
| C10m1-k1000 | 19 | 13 | +6 [+6, +6] | 0 | **miss** |
| G1-uniform | 14 | 11 | +3 [+3, +3] | 0 | **miss** |
| G1-divergent | 14 | 11 | +3 [+3, +3] | 0 | **miss** |
| G2-uniform | 14 | 11 | +3 [+3, +3] | 0 | **miss** |
| G2-divergent | 14 | 11 | +3 [+3, +3] | 0 | **miss** |
| frame | 157 | 172 | -15 [-15, -15] | 0 | pass |

**Pipelines.** No pipeline was created and no native creation call was made after warm-up in any
process of any configuration.

**Departures during the round.** None in the code or the instrumentation: R's per-operation counts
are those the amendment states, and the backend made every departure it lists. The one departure
from the procedure is the other session's load, above.

## Residual diagnosis

A report-only diagnosis, made before round 2 on a scratch build of the branch that became pull
request #70, attributes what remains. Its paired batches of eight processes ran under a measurement
lock; they were not pre-registered, only deltas within one batch are compared, and its outputs are
not kept with the evidence. It expected each miss below before the round ran.

- **C1 to C3 at `K` 100: the fixed cost per list and per pass.** 5% of C1's N at `K` 100 allows
  about 640 ns a frame, and the per-draw residual takes about 350 of them. What R still adds once a
  frame is about 0.7 µs: the pass's timestamps (the pair's claim and its two encoder
  `writeTimestamp` calls, about 190 ns), the pass's debug group with its pool (about 50 ns), and
  list acquisition and pass setup (the pool's mutex, the list's reset, the descriptor's
  configuration and its pool, the finish and close bookkeeping, about 450 ns), which a profile at
  14 ns a sample does not separate further.
- **C2 and C3 at `K` 1,000 and above: the per-draw residual.** About 2.7 ns of C2's 4.5 to 5.0 ns
  are entry code (the core's counters, the held root's compare, the arena bump and the pool flag's
  test). C3's 5.4 to 5.9 ns add `setPipeline` per draw, its held-state compares and the pool
  lookup. The draw form's share is not settled: reverting the short forms moved C3-k10000 by −1.2
  ns [−2.3, +1.1], and the rest of C3-k10000's rise after the fixes is not attributed, with an
  inlining or layout change around `setPipeline` and `draw` the likely cause.
- **C6: barrier bookkeeping per operation.** With the encoder split gone, about 6 to 7 ns per
  operation remain in `CommandList::barrier`'s lowering (the pass over the records, the stage
  translation, the queue-stage masks and the encoder tests) and in the compute encoder's checks
  before each dispatch; at `K` 100 the fixed per-list cost adds to it. Emitting the encoder barrier
  at once, in N's order, gained nothing (−0.5 ns [−1.0, +0.1]).
- **C5-k10000: the two documented counters.** Each upload raises `frameDataUploads` and
  `frameArenaBytes`; N's whole upload is 5 ns, so 5% is about one cycle, and the miss is 0.27 ns
  [0.02, 0.30]. C5 at `K` 1,000 and 100,000 pass on the same code.
- **C1-k100000: marginal.** +3.2 ns against a budget of 3.1, from C1's per-draw entry code (the
  counters, the held root's compare, the encoder and pool-flag tests); C1 passes at `K` 1,000 and
  10,000. The load note above applies.
- **Allocations.** Known from the diagnostic allocation stacks of the fixes (not this round): every
  workload pays 2 a frame for the frame event's `signalEvent`, which stays by decision, since a
  caller fence's value can be raised early by a later, unordered signal on another queue, and a
  feedback handler in its place costs at least 4 allocations a graphics commit. C1 to C3 and C10 add
  the 2 of a render pass's encoder timestamps and 1 for the `NSData` of the frame's timestamp
  resolve, which accounts for their +5. Moving the resolve to `scopeTimings` was dropped, because
  `beginFrame` reuses the retired slot's heaps in the same call. C5, C7 and C9 have the frame event
  alone. Not known: the third of C4, G1 and G2, the two beyond the frame event in C6, and C10m1's
  one or two beyond C10's five were not traced after the fixes. Before them, stacks found the carry
  fence's waits and updates about 1 each and label conversions (a short label's `UTF8String`) 1
  each; encoder labels are now set only for tools, and the command buffer's only when its text
  changes, so which of these remain is open.

## Decisions

- The CPU and allocation budgets are missed. Under the note's rules each miss is a defect in the
  backend, to be fixed and measured by a later round; no budget changes. That includes the misses
  the diagnosis expected, the fixed per-pass cost of timed render passes and the frame event among
  them.
- Texture indexing is not reopened (issue #30): the composed frame meets its GPU budget.
- No pipeline variant is hidden, and every hot path stays within its native-call budget; C9 now
  has a margin of one call.
- No mapping row of the reference changes: every stated cost class holds. Chapter 14's timestamp
  row records that an encoder's `writeTimestamp` costs nothing measurable, while the diagnosis's
  stacks count an allocation for each; the row's class, cheap, holds, and the change that removes
  or confirms those allocations settles its wording.
