# The D3D12 backend's overhead against native code: results

**Status**: Accepted (2026-11-12)

These are the results of the T2 round that [the pre-registration](2026-10-20-d3d12-overhead.md)
fixed before anything was timed, judged by its decision rules, which adopt the Metal note's, and by
[ADR 0025](../decisions/0025-cpu-budget-per-call-floor.md)'s floor of 5 ns per RojoRHI call, with
both amendments of 2026-11-12. That note's Results section summarises them; this page holds every
row. It is the first D3D12 round on hardware; [the hardware run](2026-11-12-d3d12-hardware-run.md)
records T1 on the same machine.

## Measurement

Measured on 2026-11-12 from 18:41 to 21:03 CST on an NVIDIA GeForce RTX 3080 Ti (vendor `0x10DE`,
device `0x2208`, driver 621.14, 32.0.16.2114, feature level 12_2, shader model 6.9, GPU timestamps
at 1 GHz) with an Intel Core i7-8700K under Windows 11 build 26300, MSVC 19.52.37029, Agility SDK
1.619.6, DXC 1.9.2607 and Slang 2026.14.1, on AC power with the High-performance power plan, from a
clean checkout of 6ee1823, the commit that merged the note's second amendment. The harness does not
record hardware-accelerated GPU scheduling; `HwSchMode` is unset on the machine, so Windows' default
applies. `QueryPerformanceCounter` ticks at 10 MHz, so a CPU span resolves to 100 ns, and the GPU's
timestamps come back on a grid of about 1 µs.

The correctness gate ran at that commit before anything was timed and passed every workload in
Release and in Debug with validation and the debug layer in both arms. Of the 1,260 timed processes
(63 configurations of 20), 22 attempts were discarded under the thermal rule, every one for a clock
drop (an end SM clock of 210 to 1,245 MHz, below 95% of the round's median start clock of 1,800
MHz, with a temperature rise of at most 4.3 °C against the rule's 10): 9 of C4 at `K` 100,000, 11
of C8 at `K` 10,000 and 2 of C7 at `K` 100,000. C7's reruns were nominal; C4 at `K` 100,000 and C8
at `K` 10,000 each had a process discarded four times and are reported not run. These are the
longest processes of the round, 14 to 24 s each. Nothing else was built, tested or timed on the machine during the round; Wallpaper Engine and
Dropbox were stopped, and no overlay was running. Raw results, logs, `summary.json`, `analysis.txt`
and a SHA-256 manifest are kept outside the published tree, under
`rojo-rhi-evidence/t2-d3d12/2026-11-12/` (`SHA256SUMS` SHA-256
`50461b5485172a0f60ced820c170de934d6dc8f19051da8802983fd74ac9f0c0`).

## Verdict

The round misses one of the five budgets.

| Budget | Verdict | Measured |
|---|---|---|
| CPU tax per operation, at most the larger of 5% and 5 ns per call | **missed** | 19 of the 32 judged configurations miss: C2, C4, C6 and C8 at every judged `K` that ran, C1 and C10 at `K` 100 and 1,000, C3 and C5 at `K` 100 only. At the largest `K` that ran, R adds 14.5 ns to C1 (budget 15), 18.1 to C2 (15), 16.3 to C4 (15), 3.0 to C5 (5), 47.7 to C6 (22.6), 727 to each C8 pass (62.5) and 9.2 to a C10 draw (6.3). C3 above `K` 100, C7 and C9 are within. By the 5% rule, reported, 24 miss |
| GPU frame-time tax, composed frame, at most 1% | met | R 771.2 µs, N 773.0 µs: −1.2 µs [−35.5, 35.3], −0.16% |
| Hidden pipeline variants, zero | met | no pipeline and no native creation after warm-up in any process of any configuration that ran |
| Per-frame heap allocations, R at most N | met | 0 against 0 in every configuration that ran, and RojoRHI's own `cpuAllocations` is 0 |
| Native calls, at most N plus `c` | met | R makes N's count in C1 to C7 and C9, and 8.008 against 8 (limit 15) in C8; counts are stable in every frame and process |

## Rows

**CPU and GPU time**, per configuration: CPU nanoseconds per operation and GPU microseconds per
frame, the median of process medians for R and N, and the median of the paired deltas R minus N with
its 95% bootstrap interval. The relative column is the delta over N's median, and the budget is the
larger of 5% of N and 5 ns times `c`. CPU is judged at `K` of 100 and above for C1 to C10, GPU on
the composed frame only; every other cell is reported. A pass marked "5%: miss" is within ADR 0025's
floor but not within 5%. The composed frame has no CPU span.

| Id | CPU R, ns/op | CPU N | Delta [95% interval] | Rel. | Budget | CPU rule | GPU R, µs | GPU N | Delta, µs [95% interval] | Rel. | GPU rule |
|---|---:|---:|---|---:|---:|---|---:|---:|---|---:|---|
| C1-k1 | 20,100.0 | 17,700.0 | 2,500.0 [2,350.0, 2,675.0] | +14.1% | - | reported | 2.0 | 2.0 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C1-k10 | 2,050.0 | 1,767.5 | 280.0 [245.0, 297.5] | +15.8% | - | reported | 2.0 | 2.0 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C1-k100 | 240.0 | 199.5 | 39.0 [37.0, 41.0] | +19.5% | 15.0 | **miss** | 2.0 | 2.3 | -0.3 [-1.0, 0.0] | -11.1% | reported |
| C1-k1000 | 57.6 | 40.3 | 17.4 [17.1, 17.7] | +43.2% | 15.0 | **miss** | 12.3 | 12.3 | 0.0 [0.0, 0.5] | +0.0% | reported |
| C1-k10000 | 39.5 | 24.7 | 14.6 [14.3, 15.3] | +59.0% | 15.0 | pass (5%: miss) | 114.7 | 114.7 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C1-k100000 | 37.2 | 22.5 | 14.5 [14.4, 15.1] | +64.6% | 15.0 | pass (5%: miss) | 1,130.5 | 1,130.5 | 0.0 [-1.0, 0.0] | +0.0% | reported |
| C2-k1 | 20,100.0 | 17,650.0 | 2,400.0 [2,250.0, 2,600.0] | +13.6% | - | reported | 2.0 | 2.0 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C2-k10 | 2,065.0 | 1,800.0 | 270.0 [255.0, 290.0] | +15.0% | - | reported | 2.0 | 2.0 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C2-k100 | 245.5 | 202.0 | 42.0 [40.0, 43.5] | +20.8% | 15.0 | **miss** | 2.0 | 3.1 | -1.0 [-1.0, 0.0] | -33.3% | reported |
| C2-k1000 | 61.9 | 41.1 | 21.3 [20.2, 21.8] | +51.8% | 15.0 | **miss** | 13.3 | 13.3 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C2-k10000 | 43.8 | 25.0 | 18.6 [18.2, 19.2] | +74.1% | 15.0 | **miss** | 120.8 | 120.8 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C2-k100000 | 41.0 | 23.2 | 18.1 [18.0, 18.5] | +78.3% | 15.0 | **miss** | 1,191.9 | 1,191.9 | 0.0 [-1.0, 0.0] | +0.0% | reported |
| C3-k1 | 21,700.0 | 19,200.0 | 2,500.0 [2,350.0, 3,000.0] | +13.0% | - | reported | 1.0 | 1.0 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C3-k10 | 2,340.0 | 2,080.0 | 267.5 [250.0, 300.0] | +12.9% | - | reported | 3.1 | 3.1 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C3-k100 | 394.0 | 368.0 | 27.0 [24.5, 31.0] | +7.3% | 18.4 | **miss** | 25.6 | 25.6 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C3-k1000 | 199.8 | 203.6 | -1.7 [-4.0, 1.1] | -0.8% | 10.2 | pass | 185.3 | 185.3 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C3-k10000 | 176.8 | 181.2 | -4.3 [-5.1, -3.0] | -2.4% | 10.0 | pass | 1,710.3 | 1,700.9 | 0.0 [-0.3, 19.5] | +0.0% | reported |
| C3-k100000 | 176.1 | 178.4 | -2.5 [-3.0, -1.0] | -1.4% | 10.0 | pass | 17,004.5 | 16,999.4 | 7.2 [3.3, 10.2] | +0.0% | reported |
| C4-k1 | 16,000.0 | 15,100.0 | 900.0 [800.0, 1,100.0] | +6.0% | - | reported | 4.1 | 4.1 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C4-k10 | 1,780.0 | 1,660.0 | 110.0 [95.0, 125.0] | +6.6% | - | reported | 4.1 | 4.1 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C4-k100 | 350.0 | 322.0 | 27.5 [24.2, 30.0] | +8.5% | 16.1 | **miss** | 9.2 | 9.2 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C4-k1000 | 208.7 | 189.5 | 18.2 [16.9, 19.3] | +9.6% | 15.0 | **miss** | 62.5 | 62.5 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C4-k10000 | 186.2 | 170.5 | 16.3 [14.6, 20.8] | +9.5% | 15.0 | **miss** | 600.1 | 599.0 | 1.0 [0.0, 1.0] | +0.2% | reported |
| C4-k100000 | - | - | not run | - | - | not run | - | - | not run | - | not run |
| C5-k1 | 12,500.0 | 11,500.0 | 900.0 [825.0, 1,000.0] | +7.8% | - | reported | 0.0 | 0.0 | 0.0 [0.0, 0.0] | - | reported |
| C5-k10 | 1,260.0 | 1,150.0 | 110.0 [80.0, 127.5] | +9.6% | - | reported | 0.0 | 0.0 | 0.0 [0.0, 0.0] | - | reported |
| C5-k100 | 133.0 | 120.0 | 14.0 [13.0, 15.0] | +11.7% | 6.0 | **miss** | 0.0 | 0.0 | 0.0 [0.0, 0.0] | - | reported |
| C5-k1000 | 20.9 | 16.9 | 4.1 [3.8, 4.2] | +24.0% | 5.0 | pass (5%: miss) | 0.0 | 0.0 | 0.0 [0.0, 0.0] | - | reported |
| C5-k10000 | 9.7 | 6.5 | 3.1 [3.0, 3.2] | +47.9% | 5.0 | pass (5%: miss) | 0.0 | 0.0 | 0.0 [0.0, 0.0] | - | reported |
| C5-k100000 | 8.5 | 5.5 | 3.0 [2.8, 3.0] | +53.6% | 5.0 | pass (5%: miss) | 0.0 | 0.0 | 0.0 [0.0, 0.0] | - | reported |
| C6-k1 | 18,900.0 | 18,700.0 | 225.0 [0.0, 325.0] | +1.2% | - | reported | 6.1 | 5.1 | 1.0 [1.0, 1.0] | +20.0% | reported |
| C6-k10 | 2,360.0 | 2,300.0 | 60.0 [50.0, 85.0] | +2.6% | - | reported | 25.6 | 23.6 | 2.0 [2.0, 2.0] | +8.7% | reported |
| C6-k100 | 712.0 | 656.0 | 61.2 [50.8, 67.2] | +9.3% | 32.8 | **miss** | 186.9 | 177.2 | 10.2 [9.2, 10.2] | +5.8% | reported |
| C6-k1000 | 527.1 | 479.5 | 47.2 [44.4, 49.5] | +9.8% | 24.0 | **miss** | 1,676.3 | 1,595.6 | 78.1 [71.7, 89.1] | +4.9% | reported |
| C6-k10000 | 503.0 | 454.7 | 48.7 [47.5, 49.9] | +10.7% | 22.7 | **miss** | 16,703.7 | 15,995.9 | 718.3 [699.9, 784.4] | +4.5% | reported |
| C6-k100000 | 500.5 | 451.9 | 47.7 [47.0, 49.8] | +10.6% | 22.6 | **miss** | 166,812.2 | 159,905.8 | 7,156.0 [6,826.8, 8,551.4] | +4.5% | reported |
| C7-k1 | 14,100.0 | 13,200.0 | 925.0 [850.0, 1,100.0] | +7.0% | - | reported | 1.0 | 1.0 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C7-k10 | 1,767.5 | 1,660.0 | 110.0 [95.0, 122.5] | +6.6% | - | reported | 1.0 | 1.0 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C7-k100 | 507.0 | 494.0 | 12.5 [11.0, 17.8] | +2.5% | 24.7 | pass | 3.1 | 3.1 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C7-k1000 | 384.7 | 379.9 | 4.2 [1.5, 7.8] | +1.1% | 19.0 | pass | 27.6 | 27.6 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C7-k10000 | 363.3 | 360.4 | 2.4 [1.5, 3.8] | +0.7% | 18.0 | pass | 265.2 | 265.2 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C7-k100000 | 364.7 | 362.9 | 1.7 [0.8, 3.0] | +0.5% | 18.1 | pass | 7,995.6 | 7,991.3 | 3.6 [-3.8, 7.9] | +0.0% | reported |
| C8-k1 | 20,200.0 | 18,100.0 | 2,100.0 [1,950.0, 2,300.0] | +11.6% | - | reported | 1.0 | 1.0 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C8-k10 | 3,995.0 | 3,062.5 | 930.0 [910.0, 952.5] | +30.4% | - | reported | 8.2 | 8.2 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C8-k100 | 2,209.2 | 1,436.5 | 767.0 [728.8, 837.5] | +53.4% | 71.8 | **miss** | 79.9 | 79.9 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C8-k1000 | 1,977.0 | 1,250.7 | 727.1 [725.1, 733.0] | +58.1% | 62.5 | **miss** | 803.8 | 803.6 | 1.0 [0.0, 1.0] | +0.1% | reported |
| C8-k10000 | - | - | not run | - | - | not run | - | - | not run | - | not run |
| C9-k1 | 21,175.0 | 20,375.0 | 1,000.0 [700.0, 1,200.0] | +4.9% | - | reported | 1.0 | 1.0 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C9-k10 | 15,167.5 | 14,962.5 | 350.0 [-242.5, 940.0] | +2.3% | - | reported | 128.8 | 127.0 | 4.1 [-2.8, 8.7] | +3.2% | reported |
| C9-k100 | 14,839.8 | 14,653.5 | 173.2 [91.0, 305.0] | +1.2% | 732.7 | pass | 1,462.0 | 1,443.3 | 16.4 [9.7, 32.3] | +1.1% | reported |
| C10-k1 | 21,800.0 | 19,400.0 | 2,500.0 [2,300.0, 2,650.0] | +12.9% | - | reported | 7.2 | 7.2 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C10m1-k1 | 21,750.0 | 19,400.0 | 2,350.0 [2,075.0, 2,700.0] | +12.1% | - | reported | 2.0 | 1.0 | 1.0 [1.0, 1.0] | +100.0% | reported |
| C10-k10 | 2,290.0 | 2,040.0 | 260.0 [240.0, 280.0] | +12.7% | - | reported | 62.7 | 62.5 | 0.3 [0.0, 1.0] | +0.4% | reported |
| C10m1-k10 | 2,270.0 | 2,040.0 | 230.0 [215.0, 245.0] | +11.3% | - | reported | 2.0 | 2.0 | 0.0 [0.0, 0.0] | +0.0% | reported |
| C10-k100 | 393.2 | 359.5 | 33.5 [31.0, 35.5] | +9.3% | 18.0 | **miss** | 581.9 | 580.6 | 0.5 [-41.0, 42.0] | +0.1% | reported |
| C10m1-k100 | 315.5 | 285.0 | 31.0 [28.8, 34.5] | +10.9% | - | reported | 6.1 | 5.1 | 1.0 [1.0, 1.0] | +20.0% | reported |
| C10-k1000 | 135.3 | 126.0 | 9.2 [8.7, 9.9] | +7.3% | 6.3 | **miss** | 5,764.1 | 5,754.9 | 9.2 [9.2, 9.2] | +0.2% | reported |
| C10m1-k1000 | 119.0 | 108.6 | 10.4 [9.4, 11.7] | +9.6% | - | reported | 41.0 | 39.9 | 1.0 [1.0, 1.0] | +2.6% | reported |
| G1-uniform | 15,900.0 | 14,600.0 | 1,300.0 [1,250.0, 1,400.0] | +8.9% | - | reported | 68.1 | 35.8 | 31.7 [29.7, 32.8] | +88.6% | reported |
| G1-divergent | 19,750.0 | 18,150.0 | 1,525.0 [1,000.0, 2,500.0] | +8.4% | - | reported | 366.6 | 372.7 | -6.1 [-31.7, 8.2] | -1.6% | reported |
| G2-uniform | 15,850.0 | 15,100.0 | 750.0 [700.0, 850.0] | +5.0% | - | reported | 79.4 | 74.2 | 4.6 [-4.1, 8.2] | +6.2% | reported |
| G2-divergent | 20,100.0 | 19,100.0 | 950.0 [800.0, 1,100.0] | +5.0% | - | reported | 2,165.8 | 2,161.7 | 4.1 [3.1, 5.1] | +0.2% | reported |
| frame | - | - | - | - | - | reported | 771.2 | 773.0 | -1.2 [-35.5, 35.3] | -0.2% | pass |

G2's arm F, for the record: 77.8 µs against N's 74.2 µs uniform (3.6 µs [0.0, 6.1], +4.8%) and
2,161.7 against 2,161.7 µs divergent (−0.5 µs [−1.0, 0.0], −0.0%).

**Native calls per repetition**, `(count at K = 1,000 − count at K = 100) / 900`, for C9 over `K`
100 and 10. Every count was the same in every measured frame of every process.

| Workload | R | N | Limit, N plus `c` | `c` | Rule |
|---|---:|---:|---:|---:|---|
| C1 | 2 | 2 | 5 | 3 | pass |
| C2 | 2 | 2 | 5 | 3 | pass |
| C3 | 2 | 2 | 4 | 2 | pass |
| C4 | 2 | 2 | 5 | 3 | pass |
| C5 | 0 | 0 | 1 | 1 | pass |
| C6 | 2 | 2 | 4 | 2 | pass |
| C7 | 1 | 1 | 2 | 1 | pass |
| C8 | 8.008 | 8 | 15 | 7 | pass |
| C9 | 2 | 2 | 3 | 1 | pass |
| C10 | 1 | 1 | - | 1 | reported |
| C10 at `maxCount` 1 | 1 | 1 | - | - | reported |

C8's fraction is 7 calls more over the 900 passes between `K` 100 and 1,000.

**Allocations per frame**, the C++ allocations of the process through the replaced global
`operator new`, as the note's metric 3 counts them: the median of process medians is 0 for R and 0
for N in every configuration that ran, with intervals of [0, 0], and R's own `cpuAllocations` is 0
in every one. Every configuration passes; C10 at `maxCount` 1 is reported.

**Pipelines.** No pipeline was created and no native creation call was made after warm-up in any
process of any configuration that ran.

**Departures during the round.** None. The round ran as pre-registered, with both amendments, from
the commit the second names.

## What the rows show

The round was not followed by a diagnosis; this section reads the rows alone and attributes
nothing to code.

- **A fixed cost per list.** At `K` 1 R's span exceeds N's by 2.1 to 2.5 µs in the workloads that
  open a render pass (C1, C2, C3, C8 and C10) and by 0.9 µs in the compute and copy workloads (C4,
  C5 and C7). A hundredth of it is most of the `K` 100 deltas: C3 and C5 miss at `K` 100 only (27.0
  and 14.0 ns, where their deltas at `K` 100,000 are −2.5 and 3.0 ns), and the `K` 100 deltas of
  C1, C2 and C4 exceed their large-`K` deltas by 25, 24 and 11 ns.
- **A cost per operation.** At the largest `K` that ran the deltas are 14.5 ns for C1's upload,
  root view and draw, 18.1 for C2's indexed draw, 16.3 for C4's dispatch, 3.0 for C5's upload alone,
  47.7 for C6's dispatch and global barrier, 1.7 for C7's copy and 9.2 for C10's counted draw;
  C3's pipeline change and draw are 2.5 ns faster than N's.
- **The render pass.** C8's pass, with its event pair, two timestamps, texture barrier and draw,
  costs R 727 ns more than N's 1,251 ns at `K` 1,000, with the same native calls but for the
  fraction above. It is the largest delta of the round, about twelve times its budget.
- **GPU, reported.** The composed frame, the judged GPU row, is within. C6's GPU time is 4.5% over
  N's at every large `K`, about 72 ns of GPU time per dispatch and barrier. G1 at the uniform mode
  is 88.6% over N, 31.7 µs: N's kernel reads through a root SRV with no descriptor fetch, which is
  [spike 1](2026-10-20-spike-1-d3d12-addr-dereference.md#results)'s costly verdict seen at the
  module level; G1 divergent, G2 and both arms of F are within 6.2%.

## Decisions

- The CPU budget is missed. Under the note's rules the miss is a defect in the D3D12 backend: it
  is fixed and the round runs again as a new note. The fix starts from a diagnosis of R's fixed cost
  per list, its render-pass begin and end, and its per-operation path for draws, dispatches and
  global barriers, measured by the next round, not by this one.
- Heap indexing and `Addr` dereference are not reopened: the composed frame meets its GPU budget, so
  the condition for reopening chapter 05's D3D12 rows and spike 1 does not hold.
- No pipeline variant is hidden, every hot path stays within its native-call budget, and R
  allocates nothing per frame.
- C4 at `K` 100,000 and C8 at `K` 10,000 are not run under the thermal rule, which fired on the end
  clock of the round's longest processes with no matching rise in temperature. The next round runs
  them under the same rule unless an amendment of its own pre-registration changes it before any
  timed run.
- No mapping's stated cost is shown wrong: the misses are R's CPU time with N's native calls, so the
  API reference is unchanged.
