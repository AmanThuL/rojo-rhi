# Performance against native code

RojoRHI's CPU and GPU cost is measured against hand-written native code doing the same work. Each
workload is written twice, once with RojoRHI and once directly against the native API, and both run
on the same machine in Release builds with validation off. A round's budgets and statistics are
fixed before anything is timed. Each configuration runs as 20 separate processes. Each arm's figure
is the median of its process medians, and the difference is the median of the paired differences,
RojoRHI minus native, with its 95% bootstrap interval.

A workload meets its CPU budget when RojoRHI's added cost per operation, and its added fixed cost
per command list, are each at most the larger of 5% of the native figure and 5 ns per RojoRHI call;
a miss needs the interval to exclude zero and the median to exceed the budget. The other budgets:
the composed frame's GPU time is at most 1% above native, RojoRHI makes no more heap allocations per
frame than the native code, at most one more native call than the native code for each RojoRHI call,
and no pipeline or native object after warm-up.

This page holds the latest judged round of each backend, with every budget it missed.

## Metal 4: round 5

Measured on 2026-11-17 on an Apple M4 Pro under macOS 27.2.1, on a development build between
v1.0.0 and v1.0.1.

| Budget | Result | Measured |
|---|---|---|
| CPU, per operation | met | every workload whose cost per operation could be separated meets it, at most 2.00 ns above native; the submission, judged by the earlier per-configuration rule, meets it (6.67 ns against 15.10); the render pass could not be judged apart |
| CPU, fixed cost per command list | **missed** in 2 of 8 workloads | a 256-byte upload: 41 ns against a budget of 21 (one tick of the host clock); a counted indirect draw: 355 ns against 296 |
| GPU time, composed frame | met as judged; to be re-measured | 2,136 µs against 2,147 µs. The native frame writes its render passes' timestamps on the command buffer, the costly form, where RojoRHI writes them on the encoders. That favours RojoRHI by an amount not yet measured, so the next round judges this budget against a corrected native frame. |
| Heap allocations per frame | **missed** in 1 configuration | submitting a recorded list, 100 submissions a frame: 1,577 against 1,572; equal to native everywhere else but the render pass and the composed frame, where the native code's costly timestamp writes allocate more; RojoRHI's own allocations are 0 in every workload |
| Native calls | met | within the limit in every judged workload; the counted indirect draw is reported, not judged |
| Pipelines and native objects after warm-up | met | none in any workload |

Selected workloads, CPU time per operation:

| Workload | RojoRHI | Native Metal 4 | Difference |
|---|---:|---:|---:|
| Per draw: a 64-byte root block and a draw, 1,000 a list | 67.6 ns | 66.0 ns | +2.6% |
| Per pipeline switch and draw, 100 a list | 119.4 ns | 116.0 ns | +3.7% |

## Direct3D 12: round 1

The Direct3D 12 backend passes the conformance suite on one GPU, an NVIDIA GeForce RTX 3080 Ti on
driver 621.14; other vendors' GPUs are not tested. Its CPU cost was above the project's budgets
when last measured, in the round below. That round judged CPU cost per configuration, with no
separate fixed cost per command list.

Measured on 2026-11-12 on an NVIDIA GeForce RTX 3080 Ti, driver 621.14, with an Intel Core
i7-8700K under Windows 11 build 26300, on a development build before v1.0.0.

| Budget | Result | Measured |
|---|---|---|
| CPU, per operation | **missed** in 19 of 32 configurations | eight of the ten workloads miss at some batch size; only the copy and the submission are within everywhere. At the largest batch that ran, RojoRHI adds 18.1 ns per indexed draw (15), 16.3 ns per dispatch with a root block (15), 47.7 ns per dispatch and barrier (22.6), 727 ns per render pass (62.5) and 9.2 ns per counted indirect draw (6.3); a draw with a root block misses at 100 and 1,000 draws a list and is within at the largest, 14.5 ns against 15 |
| GPU time, composed frame | met | 771.2 µs against 773.0 µs; the native frame issues the same barriers as RojoRHI, which the next round reviews |
| Heap allocations per frame | met | counted as C++ `operator new` calls, not the Metal measure: 0 against 0 in every configuration that ran; two did not run, the dispatch with a root block at 100,000 a list and the render pass at 10,000 |
| Native calls | met | equal to native in all but the render pass, 8.008 against 8 (limit 15) |
| Pipelines and native objects after warm-up | met | none in any configuration |

Work on the CPU cost of the Direct3D 12 backend continues, and its next round replaces this table.
