# The Metal 4 backend's overhead against native code: results

**Status**: Accepted (2026-10-19)

These are the results of the T2 round that [the pre-registration](2026-10-18-metal4-overhead.md)
fixed before anything was timed, judged by its decision rules as amended there on 2026-10-18 and
2026-10-19. That note's Results section summarises them; this page holds every row.

## Measurement

Measured on 2026-10-19 from 14:04 to 17:45 CDT on an Apple M3 Max (Apple7 to Apple9, Metal 4,
timestamps at 24 MHz) under macOS 27.0.1 (26A434), Xcode 26.6, `metal` 32023.883, Apple clang 21.0.0
and `metal-cpp` c595afe, on AC power, from a clean checkout of 4f2e0d6, the commit that merged the
note's second amendment. The correctness gate ran again at that commit before anything was timed and
passed every workload in Release and in Debug with validation and `MTL_DEBUG_LAYER=1`. All 1,260
timed processes (63 configurations of 20) started and ended at the nominal thermal state on their
first attempt: none was discarded, no configuration is reported not run, and no process printed a
line on standard error, so no scope went untimed. Nothing else was built, tested or timed on the
machine during the round. Raw results, logs, the driver's output, `summary.json`, `analysis.txt` and
a SHA-256 manifest are kept outside the published tree, under `rojo-rhi-evidence/t2/2026-10-19/`.

## Verdict

The round misses three of the five budgets.

| Budget | Verdict | Measured |
|---|---|---|
| CPU tax per operation, at most 5% | **missed** | 33 of the 34 judged configurations miss: C1 to C7 by 23% to 68% at every `K` from 100 (at `K` 100,000, +22 to +30 ns per operation of C1 to C4, C6 and C7, and +2.2 ns, 50%, for C5's upload alone), C8 by 9.5% and 8.1% at `K` 1,000 and 10,000, C9 by 21% at `K` 100, C10 by 7.1% and 10.7%; C8 at `K` 100 is within (+4.98%) |
| GPU frame-time tax, composed frame, at most 1% | **missed** | R 2,048.8 µs, N 1,961.5 µs: +85.6 µs [82.7, 89.6], +4.37% |
| Hidden pipeline variants, zero | met | no pipeline and no native creation after warm-up in any process of any configuration |
| Per-frame heap allocations, R at most N | **missed** | R allocates 2 to 10 more per frame than N in 61 of 63 configurations (C9 at `K` 100: +216, about 2 per submit; the composed frame: 187 against 172); C8 at `K` 1,000 and 10,000 are within; RojoRHI's own `cpuAllocations` is 0 everywhere |
| Native calls, at most N plus `c` | met | every operation of C1 to C9 is within, with stable counts in every frame and process; C8 makes 14 against a limit of 19, and C9 makes 3 against a limit of 3, with no margin |

## Rows

**CPU and GPU time**, per configuration: CPU nanoseconds per operation and GPU microseconds per
frame, the median of process medians for R and N, and the median of the paired deltas R minus N with
its 95% bootstrap interval. The relative column is the delta over N's median. CPU is judged at `K`
of 100 and above for C1 to C10, GPU on the composed frame only; every other cell is reported. The
composed frame has no CPU span.

| Id | CPU R, ns/op | CPU N | Delta [95% interval] | Rel. | CPU rule | GPU R, µs | GPU N | Delta, µs [95% interval] | Rel. | GPU rule |
|---|---:|---:|---|---:|---|---:|---:|---|---:|---|
| C1-k1 | 9,427.1 | 5,041.7 | 4,375.0 [4,291.7, 4,395.8] | +86.8% | reported | 108.6 | 14.3 | 95.2 [87.4, 96.9] | +668.3% | reported |
| C1-k10 | 1,062.5 | 587.5 | 470.8 [466.7, 486.5] | +80.1% | reported | 37.6 | 14.3 | 23.3 [22.3, 76.4] | +162.8% | reported |
| C1-k100 | 194.0 | 121.7 | 72.7 [71.5, 73.3] | +59.8% | **miss** | 19.6 | 15.9 | 3.8 [3.5, 3.9] | +23.7% | reported |
| C1-k1000 | 96.7 | 70.3 | 26.0 [25.7, 26.6] | +36.9% | **miss** | 94.3 | 61.5 | 33.0 [32.6, 33.1] | +53.6% | reported |
| C1-k10000 | 81.8 | 60.2 | 21.7 [21.2, 22.2] | +36.0% | **miss** | 662.1 | 146.0 | 516.3 [515.2, 516.8] | +353.6% | reported |
| C1-k100000 | 81.1 | 58.1 | 22.2 [21.7, 23.3] | +38.2% | **miss** | 5,187.6 | 1,048.8 | 4,138.2 [4,136.5, 4,142.1] | +394.6% | reported |
| C2-k1 | 9,447.9 | 5,083.3 | 4,354.2 [4,312.5, 4,416.7] | +85.7% | reported | 111.8 | 14.5 | 97.6 [88.0, 99.2] | +673.0% | reported |
| C2-k10 | 1,086.5 | 587.5 | 493.8 [489.6, 502.1] | +84.0% | reported | 36.2 | 14.3 | 22.1 [21.4, 23.4] | +155.0% | reported |
| C2-k100 | 202.5 | 123.3 | 79.2 [78.5, 80.9] | +64.2% | **miss** | 19.0 | 15.5 | 3.6 [3.3, 3.7] | +22.9% | reported |
| C2-k1000 | 103.3 | 71.5 | 32.4 [30.7, 32.9] | +45.3% | **miss** | 104.9 | 61.9 | 42.9 [42.6, 43.1] | +69.2% | reported |
| C2-k10000 | 88.7 | 61.4 | 27.7 [27.0, 28.9] | +45.1% | **miss** | 764.2 | 146.7 | 617.3 [616.9, 618.3] | +420.8% | reported |
| C2-k100000 | 89.5 | 60.6 | 29.6 [27.9, 29.8] | +48.8% | **miss** | 6,063.4 | 1,072.0 | 4,989.7 [4,988.6, 4,993.3] | +465.5% | reported |
| C3-k1 | 9,395.8 | 5,083.3 | 4,312.5 [4,250.0, 4,375.0] | +84.8% | reported | 109.0 | 14.3 | 95.4 [92.1, 97.3] | +669.3% | reported |
| C3-k10 | 1,037.5 | 566.7 | 468.8 [462.5, 479.2] | +82.7% | reported | 39.4 | 14.3 | 24.9 [23.4, 91.7] | +174.2% | reported |
| C3-k100 | 180.0 | 107.9 | 73.3 [71.0, 74.4] | +68.0% | **miss** | 21.3 | 18.3 | 3.0 [2.6, 3.4] | +16.6% | reported |
| C3-k1000 | 85.0 | 58.2 | 26.9 [23.4, 27.8] | +46.3% | **miss** | 198.7 | 92.5 | 106.4 [105.9, 106.8] | +115.0% | reported |
| C3-k10000 | 70.8 | 49.0 | 21.8 [19.6, 24.0] | +44.6% | **miss** | 1,568.1 | 394.4 | 1,172.5 [1,166.8, 1,175.8] | +297.3% | reported |
| C3-k100000 | 70.4 | 47.1 | 23.3 [21.2, 25.3] | +49.3% | **miss** | 15,057.4 | 4,915.1 | 10,146.5 [10,106.1, 10,179.4] | +206.4% | reported |
| C4-k1 | 3,083.3 | 2,375.0 | 666.7 [625.0, 708.3] | +28.1% | reported | 2.6 | 2.4 | 0.1 [0.1, 0.3] | +5.2% | reported |
| C4-k10 | 439.6 | 337.5 | 104.2 [100.0, 106.2] | +30.9% | reported | 3.9 | 3.8 | 0.1 [0.0, 0.1] | +1.6% | reported |
| C4-k100 | 137.7 | 105.4 | 31.9 [28.8, 35.6] | +30.2% | **miss** | 24.2 | 24.2 | -0.1 [-0.1, 0.1] | -0.3% | reported |
| C4-k1000 | 96.3 | 76.2 | 19.6 [18.6, 23.8] | +25.7% | **miss** | 242.6 | 242.7 | -0.0 [-0.2, 0.2] | -0.0% | reported |
| C4-k10000 | 88.9 | 67.8 | 20.9 [18.4, 23.0] | +30.8% | **miss** | 2,333.1 | 2,325.0 | 5.8 [-1.7, 14.8] | +0.2% | reported |
| C4-k100000 | 89.5 | 67.7 | 21.7 [19.0, 24.3] | +32.0% | **miss** | 16,193.6 | 16,097.6 | 95.7 [29.1, 131.2] | +0.6% | reported |
| C5-k1 | 583.3 | 562.5 | 83.3 [-41.7, 125.0] | +14.8% | reported | 0.0 | 0.0 | 0.0 [-0.0, 0.0] | - | reported |
| C5-k10 | 66.7 | 58.3 | 10.4 [0.0, 16.7] | +17.9% | reported | 0.0 | 0.0 | 0.0 [-0.0, 0.0] | - | reported |
| C5-k100 | 12.9 | 10.6 | 2.5 [1.2, 3.8] | +23.5% | **miss** | 0.0 | 0.0 | 0.0 [-0.0, 0.0] | - | reported |
| C5-k1000 | 7.3 | 5.1 | 2.1 [1.2, 2.2] | +41.5% | **miss** | 0.0 | 0.0 | 0.0 [-0.0, 0.0] | - | reported |
| C5-k10000 | 7.2 | 4.9 | 2.3 [2.3, 2.3] | +46.6% | **miss** | 0.0 | 0.0 | 0.0 [0.0, 0.0] | - | reported |
| C5-k100000 | 6.7 | 4.5 | 2.2 [2.2, 2.4] | +50.3% | **miss** | 0.0 | 0.0 | 0.0 [0.0, 0.0] | - | reported |
| C6-k1 | 6,833.3 | 3,166.7 | 3,666.7 [3,625.0, 3,812.5] | +115.8% | reported | 6.1 | 4.2 | 1.8 [1.7, 1.9] | +43.4% | reported |
| C6-k10 | 825.0 | 418.8 | 410.4 [400.0, 425.0] | +98.0% | reported | 25.3 | 19.0 | 6.3 [6.0, 6.5] | +33.2% | reported |
| C6-k100 | 198.9 | 123.3 | 73.3 [66.1, 83.1] | +59.5% | **miss** | 222.0 | 167.9 | 54.2 [53.7, 54.9] | +32.3% | reported |
| C6-k1000 | 143.3 | 114.6 | 28.5 [23.1, 33.5] | +24.8% | **miss** | 2,086.3 | 1,579.8 | 507.6 [501.5, 510.5] | +32.1% | reported |
| C6-k10000 | 105.3 | 77.1 | 28.2 [22.6, 33.8] | +36.5% | **miss** | 20,715.2 | 15,675.5 | 5,036.5 [5,004.9, 5,050.3] | +32.1% | reported |
| C6-k100000 | 108.8 | 79.3 | 29.7 [24.3, 35.1] | +37.4% | **miss** | 160,078.2 | 126,178.9 | 34,066.6 [33,619.4, 34,481.0] | +27.0% | reported |
| C7-k1 | 3,395.8 | 2,750.0 | 625.0 [625.0, 645.8] | +22.7% | reported | 2.8 | 2.7 | 0.1 [0.0, 0.1] | +3.9% | reported |
| C7-k10 | 479.2 | 395.8 | 85.4 [79.2, 89.6] | +21.6% | reported | 4.2 | 4.1 | 0.1 [0.1, 0.1] | +3.0% | reported |
| C7-k100 | 167.1 | 132.7 | 35.2 [31.0, 36.7] | +26.5% | **miss** | 24.7 | 24.7 | 0.1 [-0.0, 0.1] | +0.3% | reported |
| C7-k1000 | 118.7 | 95.6 | 23.2 [20.8, 25.4] | +24.3% | **miss** | 248.6 | 248.6 | 0.0 [-0.2, 0.5] | +0.0% | reported |
| C7-k10000 | 111.8 | 88.2 | 23.8 [21.5, 25.3] | +27.0% | **miss** | 2,388.7 | 2,398.6 | -9.5 [-16.1, -7.1] | -0.4% | reported |
| C7-k100000 | 111.5 | 88.6 | 22.7 [20.7, 25.1] | +25.7% | **miss** | 17,290.7 | 16,921.9 | 340.5 [287.1, 391.7] | +2.0% | reported |
| C8-k1 | 9,645.8 | 8,250.0 | 1,375.0 [958.3, 1,625.0] | +16.7% | reported | 33.1 | 31.8 | 1.9 [-0.1, 4.8] | +6.0% | reported |
| C8-k10 | 8,428.1 | 8,400.0 | -20.8 [-7,737.5, 196.9] | -0.2% | reported | 705.1 | 702.6 | -34.4 [-194.9, 14.7] | -4.9% | reported |
| C8-k100 | 14,415.4 | 13,960.7 | 695.1 [111.7, 1,074.2] | +5.0% | pass | 8,196.2 | 7,448.9 | 541.8 [164.6, 856.0] | +7.3% | reported |
| C8-k1000 | 15,395.5 | 14,095.4 | 1,338.2 [345.3, 2,394.2] | +9.5% | **miss** | 89,192.6 | 96,608.3 | -8,833.4 [-20,090.2, 23,239.6] | -9.1% | reported |
| C8-k10000 | 8,677.4 | 8,038.1 | 648.3 [514.9, 843.7] | +8.1% | **miss** | 739,470.1 | 738,330.8 | 1,091.5 [-1,407.2, 3,189.3] | +0.1% | reported |
| C9-k1 | 1,916.7 | 1,864.6 | 52.1 [-31.2, 93.8] | +2.8% | reported | 2.8 | 2.6 | 0.1 [0.1, 0.2] | +4.8% | reported |
| C9-k10 | 752.1 | 612.5 | 141.7 [134.4, 145.8] | +23.1% | reported | 439.0 | 261.6 | 179.2 [176.3, 183.1] | +68.5% | reported |
| C9-k100 | 489.3 | 402.5 | 85.0 [59.5, 118.3] | +21.1% | **miss** | 4,419.1 | 3,044.4 | 1,372.0 [1,176.0, 1,395.0] | +45.1% | reported |
| C10-k1 | 13,510.4 | 7,583.3 | 5,968.8 [5,916.7, 6,041.7] | +78.7% | reported | 172.3 | 78.8 | 95.3 [93.1, 96.5] | +120.9% | reported |
| C10m1-k1 | 11,875.0 | 4,875.0 | 7,020.8 [6,937.5, 7,041.7] | +144.0% | reported | 135.5 | 13.6 | 121.9 [120.4, 122.7] | +897.1% | reported |
| C10-k10 | 4,980.2 | 3,943.8 | 1,041.7 [965.6, 1,099.0] | +26.4% | reported | 362.6 | 273.9 | 88.7 [88.1, 89.4] | +32.4% | reported |
| C10m1-k10 | 1,402.1 | 554.2 | 847.9 [842.7, 854.2] | +153.0% | reported | 135.1 | 11.9 | 123.3 [122.4, 124.2] | +1038.5% | reported |
| C10-k100 | 999.6 | 932.8 | 66.3 [58.0, 72.8] | +7.1% | **miss** | 1,752.6 | 1,683.4 | 68.1 [65.6, 70.3] | +4.0% | reported |
| C10m1-k100 | 319.2 | 104.4 | 214.8 [213.2, 217.1] | +205.8% | reported | 151.3 | 14.6 | 136.6 [135.7, 137.9] | +936.0% | reported |
| C10-k1000 | 623.7 | 565.0 | 60.2 [50.4, 64.8] | +10.7% | **miss** | 17,063.0 | 17,076.2 | 18.5 [-65.1, 32.6] | +0.1% | reported |
| C10m1-k1000 | 194.1 | 59.7 | 134.8 [134.1, 135.4] | +225.8% | reported | 367.4 | 60.1 | 307.2 [306.8, 307.8] | +511.6% | reported |
| G1-uniform | 3,000.0 | 2,416.7 | 614.6 [583.3, 625.0] | +25.4% | reported | 59.5 | 51.0 | 8.6 [8.4, 8.8] | +16.9% | reported |
| G1-divergent | 8,125.0 | 6,458.3 | 1,666.7 [1,583.3, 1,750.0] | +25.8% | reported | 429.3 | 425.7 | 3.6 [3.4, 3.8] | +0.8% | reported |
| G2-uniform | 3,041.7 | 2,416.7 | 625.0 [625.0, 625.0] | +25.9% | reported | 62.3 | 62.1 | 0.2 [0.1, 0.3] | +0.3% | reported |
| G2-divergent | 8,250.0 | 6,541.7 | 1,791.7 [1,708.3, 1,822.9] | +27.4% | reported | 1,541.9 | 1,541.7 | 0.2 [0.2, 0.3] | +0.0% | reported |
| frame | - | - | - | - | reported | 2,048.8 | 1,961.5 | 85.6 [82.7, 89.6] | +4.4% | **miss** |

G2's arm F, for the record: 57.4 µs against N's 62.1 µs uniform (−4.8 µs [−4.9, −4.7], −7.7%) and
1,540.4 against 1,541.7 µs divergent (−1.3 µs [−1.3, −1.2], −0.1%).

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
| C8 | 14 | 12 | 19 | 7 | pass |
| C9 | 3 | 2 | 3 | 1 | pass |
| C10 | 3 | 3 | - | 1 | reported |
| C10 at `maxCount` 1 | 3 | 1 | - | - | reported |

**Allocations per frame** through the default malloc zone, the median of process medians, and
RojoRHI's own `cpuAllocations` for R.

| Id | Allocations R | N | Delta [95% interval] | R `cpuAllocations` | Rule |
|---|---:|---:|---|---:|---|
| C1-k1 | 20 | 12 | +8 [+8, +8] | 0 | **miss** |
| C1-k10 | 20 | 12 | +8 [+8, +8] | 0 | **miss** |
| C1-k100 | 20 | 12 | +8 [+8, +8] | 0 | **miss** |
| C1-k1000 | 20 | 13 | +7 [+7, +7] | 0 | **miss** |
| C1-k10000 | 22 | 15 | +7 [+7, +7] | 0 | **miss** |
| C1-k100000 | 27 | 20 | +7 [+7, +7] | 0 | **miss** |
| C2-k1 | 20 | 12 | +8 [+8, +8] | 0 | **miss** |
| C2-k10 | 20 | 12 | +8 [+8, +8] | 0 | **miss** |
| C2-k100 | 20 | 12 | +8 [+8, +8] | 0 | **miss** |
| C2-k1000 | 20 | 13 | +7 [+7, +7] | 0 | **miss** |
| C2-k10000 | 23 | 15 | +8 [+8, +8] | 0 | **miss** |
| C2-k100000 | 27 | 20 | +7 [+7, +7] | 0 | **miss** |
| C3-k1 | 21 | 13 | +8 [+8, +8] | 0 | **miss** |
| C3-k10 | 21 | 13 | +8 [+8, +8] | 0 | **miss** |
| C3-k100 | 21 | 13 | +8 [+8, +8] | 0 | **miss** |
| C3-k1000 | 21 | 14 | +7 [+7, +7] | 0 | **miss** |
| C3-k10000 | 22 | 15 | +7 [+7, +7] | 0 | **miss** |
| C3-k100000 | 25 | 18 | +7 [+7, +7] | 0 | **miss** |
| C4-k1 | 13 | 11 | +2 [+2, +2] | 0 | **miss** |
| C4-k10 | 13 | 11 | +2 [+2, +2] | 0 | **miss** |
| C4-k100 | 13 | 11 | +2 [+2, +2] | 0 | **miss** |
| C4-k1000 | 14 | 12 | +2 [+2, +2] | 0 | **miss** |
| C4-k10000 | 16 | 14 | +2 [+2, +2] | 0 | **miss** |
| C4-k100000 | 21 | 19 | +2 [+2, +2] | 0 | **miss** |
| C5-k1 | 12 | 10 | +2 [+2, +2] | 0 | **miss** |
| C5-k10 | 12 | 10 | +2 [+2, +2] | 0 | **miss** |
| C5-k100 | 12 | 10 | +2 [+2, +2] | 0 | **miss** |
| C5-k1000 | 12 | 10 | +2 [+2, +2] | 0 | **miss** |
| C5-k10000 | 12 | 10 | +2 [+2, +2] | 0 | **miss** |
| C5-k100000 | 12 | 10 | +2 [+2, +2] | 0 | **miss** |
| C6-k1 | 20 | 14 | +6 [+6, +6] | 0 | **miss** |
| C6-k10 | 20 | 14 | +6 [+6, +6] | 0 | **miss** |
| C6-k100 | 20 | 14 | +6 [+6, +6] | 0 | **miss** |
| C6-k1000 | 20 | 15 | +5 [+5, +5] | 0 | **miss** |
| C6-k10000 | 22 | 17 | +5 [+5, +5] | 0 | **miss** |
| C6-k100000 | 27 | 22 | +5 [+5, +5] | 0 | **miss** |
| C7-k1 | 16 | 14 | +2 [+2, +2] | 0 | **miss** |
| C7-k10 | 16 | 14 | +2 [+2, +2] | 0 | **miss** |
| C7-k100 | 16 | 14 | +2 [+2, +2] | 0 | **miss** |
| C7-k1000 | 17 | 15 | +2 [+2, +2] | 0 | **miss** |
| C7-k10000 | 20 | 18 | +2 [+2, +2] | 0 | **miss** |
| C7-k100000 | 24 | 22 | +2 [+2, +2] | 0 | **miss** |
| C8-k1 | 20 | 18 | +2 [+2, +2] | 0 | **miss** |
| C8-k10 | 75 | 73 | +2 [+2, +2] | 0 | **miss** |
| C8-k100 | 618 | 616 | +2 [+2, +2] | 0 | **miss** |
| C8-k1000 | 6025 | 6025 | +0 [+0, +0] | 0 | pass |
| C8-k10000 | 60034 | 60051 | -18 [-18, -17] | 0 | pass |
| C9-k1 | 20 | 16 | +4 [+4, +4] | 0 | **miss** |
| C9-k10 | 175 | 152 | +23 [+23, +23] | 0 | **miss** |
| C9-k100 | 1729 | 1513 | +216 [+216, +217] | 0 | **miss** |
| C10-k1 | 24 | 14 | +10 [+10, +10] | 0 | **miss** |
| C10m1-k1 | 22 | 12 | +10 [+10, +10] | 0 | **miss** |
| C10-k10 | 24 | 14 | +10 [+10, +10] | 0 | **miss** |
| C10m1-k10 | 22 | 12 | +10 [+10, +10] | 0 | **miss** |
| C10-k100 | 24 | 14 | +10 [+10, +10] | 0 | **miss** |
| C10m1-k100 | 22 | 12 | +10 [+10, +10] | 0 | **miss** |
| C10-k1000 | 27 | 17 | +10 [+10, +10] | 0 | **miss** |
| C10m1-k1000 | 22 | 13 | +9 [+9, +9] | 0 | **miss** |
| G1-uniform | 13 | 11 | +2 [+2, +2] | 0 | **miss** |
| G1-divergent | 13 | 11 | +2 [+2, +2] | 0 | **miss** |
| G2-uniform | 13 | 11 | +2 [+2, +2] | 0 | **miss** |
| G2-divergent | 13 | 11 | +2 [+2, +2] | 0 | **miss** |
| frame | 187 | 172 | +15 [+15, +15] | 0 | **miss** |

**Pipelines.** No pipeline was created and no native creation call was made after warm-up in any
process of any configuration.

**Departures during the round.** None beyond the note's amendments, which held as stated: R's counts
equal the gate's, and the backend made every departure they list. The round ran exactly as
pre-registered, with the instrumentation as amended.

## Cause analysis

A report-only diagnosis followed the round, with ablations of the backend in a scratch build of the
same commit, per-pass GPU timestamps, Time Profiler samples and allocation stacks. Its processes
were not pre-registered, and only its paired deltas within one batch are compared; its outputs, the
patches it measured and a SHA-256 manifest are kept under
`rojo-rhi-evidence/t2-diagnosis/2026-10-19/`. It attributes each miss to the backend's lowering:

- **Composed frame, GPU.** About 70 to 85 µs of the 86 µs come from the trailing encoder that closes
  the `gbuffer` list: its queue-level barrier holds the `shadows` list's hoisted counted-draw
  kernels behind `geometry`'s fragment work, which N leaves free to overlap (about 47 µs for the
  wait and 35 µs for the extra encoder). `PixelFormatView`, which the backend adds to every texture
  with an sRGB counterpart (`albedo`, `material` and `ldr` here), costs about 20 to 45 µs in the
  passes that write them. The queue's progress signal delays the first cross-queue wait by about 20
  µs. With the trailing encoder and `PixelFormatView` ablated the frame measured −1.4% [−2.0, −0.4]
  against N.
- **Texture indexing is not the cause.** Both arms index textures through the ID table, and the
  passes that sample through it differ from N by at most 4.7 µs, except `toneMap`, whose 12 µs go
  with `PixelFormatView` alone. Issue #30 is not reopened.
- **CPU per operation.** The `NS::AutoreleasePool` object that the backend opens around every native
  draw, dispatch and copy costs 17 to 20 ns of the 22 to 30 ns per operation (issue #60); raw pool
  pushes and pops would still cost 6 to 8 ns. The remaining 5 to 7 ns per draw are an out-of-line
  arena bump, a `gpuAddress` message on the arena page (and on the index buffer in C2) and an
  out-of-line encoder check per draw. C5's 2.2 ns are the same bump and message. C6 adds about 11 ns
  at its deferred encoder barrier. C9's 85 ns are about 55 ns for the queue's progress `signalEvent`
  (issue #65) and the rest submit bookkeeping. C8's 0.8 to 1.3 µs per pass of about 14 µs is not
  attributed further: R's per-pass extras (a label string, the descriptor configuration, the
  argument table and winding calls, the timestamp claim and the prefix replay) are each within the
  diagnosis's noise.
- **CPU per frame.** About 3.7 of the 4.4 µs at `K` 1 are the two `writeTimestampIntoHeap` calls a
  render pass's scope makes on the command buffer, Metal's own cost: N with the same pair pays the
  same, while encoder-level timestamps cost nothing measurable.
- **GPU on C1 to C3, reported.** A timestamp pair written on the command buffer around a render
  encoder makes Metal run that encoder's draws about four times slower on this GPU; N with the same
  pair measured 558 µs against R's 559 µs at `K` 10,000, and R without it 151 µs against N's 150 µs.
  C6's 32% is the `ResourceAlias` visibility on the global barrier (+0.7% without it), and C9's 45%
  is the progress signal (+2.9% without it).
- **Allocations.** Every excess allocation is Metal or libdispatch work that R's extra calls
  trigger: 2 per `signalEvent` (the frame event every frame, the progress event every submit that
  waits or signals), 2 per command-buffer timestamp, 1 per extra compute encoder and 2 per list that
  binds the residency set for its indirect command buffers. RojoRHI itself allocates nothing.

## Decisions

- The CPU, GPU and allocation budgets are missed. Under the note's rules each miss is a defect in
  the backend: it is fixed and the round runs again as a new note. The diagnosis's candidate fixes
  are pools only under the validation layer, cached GPU addresses and an inlined arena bump on the
  hot path, encoder-level timestamps for scopes, no trailing encoder holding a pass's own kernels
  behind the previous list's fragment work, `PixelFormatView` only when a reinterpreting view is
  asked for, `ResourceAlias` only where memory aliases, a progress signal only when a destroyed
  fence's event waits, and no per-frame frame-event signal. Each is measured by the next round, not
  by this one.
- Texture indexing is not reopened (issue #30): the composed frame misses its GPU budget, and the
  investigation shows texture indexing is not the cause.
- No pipeline variant is hidden, and every hot path stays within its native-call budget; C9's margin
  is zero.
- The rule that the results change the reference where a mapping's stated cost was wrong applies to
  three Metal 4 mappings, which cost more than their stated class as they are written: scopes and
  render passes time through command-buffer timestamps, chapter 09 lowers every global barrier with
  `ResourceAlias`, and a texture with an sRGB counterpart carries `PixelFormatView`. Each has a
  candidate lowering that would keep its class, so this note leaves the reference unchanged and the
  fix decides between the lowering and the class, in the change that makes it.
