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

Before a round is timed, each miss it expects is registered with its cause and an upper bound. A
predicted miss that comes in within its bound is still a miss: it is published here with its cause
and tracked for a future release.

This page holds the latest judged round of each backend, with every budget it missed.


## Metal 4: round 6

Measured on 2026-11-19 on an Apple M4 Pro under macOS 27.2.1, on a development build between
v1.0.2 and v1.0.3. Every miss below was predicted, with its cause, before the round was timed, and
each came in within its predicted bound.

| Budget | Result | Measured |
|---|---|---|
| CPU, per operation | **missed** in 1 workload, as predicted | a render pass, whose fixed and per-pass costs could not be separated, so it is judged at each batch size: at 10,000 passes a list RojoRHI adds 195.79 ns a pass against a budget of 189.67 (predicted at most 300). The cause is RojoRHI's own work in its seven calls a pass; RojoRHI and the native code make the same native calls but two, the argument table's bind and the winding, which are about 80 ns of it. It meets at 1,000 passes a list, 263.95 ns against 278.40, and at 100. Every other workload meets, at most 4.76 ns above native |
| CPU, fixed cost per command list | **missed** in 1 of 8 workloads, as predicted | a 256-byte upload, timed as a batch of 64 lists: 83 ns against a budget of 18 (predicted at most 115). The cause is what a list costs outside its calls: the pool's lock and the label compare when it is acquired, its first frame-arena claim and the autorelease pool `end` opens. The counted indirect draw, which missed in round 5, meets at 135 ns against 297, but its reading moves by about 200 ns between sessions, and informal sessions the same day read 312 to 375 ns, so it is not settled |
| GPU time, composed frame | met | 2,130.39 µs against 2,145.70 µs, −0.67%; the native frame now writes its render passes' timestamps on the encoders, as RojoRHI does |
| Heap allocations per frame | **missed** in 1 configuration, as predicted | counted on the thread that records and submits: the composed frame, 146 against 145 (151 against 150 on every thread). The cause is the wait on the queue's carry fence in a compute encoder that dispatches, one call and one allocation, which the backend keeps because the fence saves GPU time. Equal to native in every other configuration, the submission that missed in round 5 included; RojoRHI's own allocations are 0 in every frame |
| Native calls | met | within the limit in every workload |
| Pipelines and native objects after warm-up | met | none in any workload |

Selected workloads, CPU time per operation:

| Workload | RojoRHI | Native Metal 4 | Difference |
|---|---:|---:|---:|
| Per draw: a 64-byte root block and a draw, 1,000 a list | 67.79 ns | 65.99 ns | +2.64% |
| Per pipeline switch and draw, 100 a list | 119.79 ns | 116.04 ns | +3.23% |

## Direct3D 12: round 2

The Direct3D 12 backend passes the conformance suite on one GPU, an NVIDIA GeForce RTX 3080 Ti on
driver 621.14; other vendors' GPUs are not tested. Its CPU cost was above the project's budgets
when last measured, in the round below.

Measured on 2026-11-20 on an NVIDIA GeForce RTX 3080 Ti, driver 621.14, with an Intel Core
i7-8700K under Windows 11 build 26300, on a development build between v1.0.2 and v1.0.3. Every
miss below was predicted, with its cause, before the round was timed, and each came in within its
predicted bound.

| Budget | Result | Measured |
|---|---|---|
| CPU, per operation | **missed** in 3 of 9 workloads, as predicted | a render pass: 108.64 ns above native against a budget of 43.86 (predicted at most 130), in the scope's and the pass's bookkeeping, the starting state every pass sets and the core's work for a barrier the backend then leaves out; a counted indirect draw: 12.00 ns against 6.01 (at most 24), in the draw's flush, the checks of its cached indices and its call count; a pipeline switch and draw: 10.89 ns against 10.00 (at most 16), in the pipeline's resolve, the topology and the draw's flush. Every other workload meets, a draw with a root block at 10.47 ns against 15 and an indexed draw at 13.28 against 15 |
| CPU, fixed cost per command list | **missed** in 4 of 9 workloads, as predicted | a draw with a root block, 1,488 ns against a budget of 1,019 (predicted at most 2,000); an indexed draw, 1,386 against 1,014 (at most 1,900); a pipeline switch and draw, 1,241 against 1,082 (at most 1,800); a render pass, 1,295 against 981 (at most 1,900), whose interval, [945, 1,585], reaches below its budget, so it is not settled either. The cause is what a list costs outside its calls: the core's list begin, the first draw's flush, the core's pass begin and the scope's begin, and `setPipeline`'s resolve and topology. Three workloads that pass, 69 to 174 ns under their budgets, lie within the spread between sessions and are not settled, as the pipeline switch's miss, 159 ns over, is not |
| GPU time, composed frame | met | 759,720 ns against 762,522 ns; the paired difference is +1,236 ns [−10,288, 37,488] against a budget of 7,625 ns, +0.16% |
| Heap allocations per frame | met | counted as C++ `operator new` calls, not the Metal measure: 0 against 0 in every configuration that ran; RojoRHI's own allocations are 0 in every frame. One configuration, a dispatch with a root block at 100,000 a list, did not run, under the thermal rule |
| Native calls | met | equal to native per repetition in every judged workload |
| Pipelines and native objects after warm-up | met | none in any configuration |

Work on the CPU cost of both backends continues, and each backend's next round replaces its table.
