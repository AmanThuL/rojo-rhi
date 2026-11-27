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

## Direct3D 12: round 3

The Direct3D 12 backend passes the conformance suite on one GPU, an NVIDIA GeForce RTX 3080 Ti on
driver 621.14; other vendors' GPUs are not tested. Its CPU cost was above the project's budgets
when last measured, in the round below.

Measured on 2026-11-26 on an NVIDIA GeForce RTX 3080 Ti, driver 621.14, with an Intel Core
i9-14900K under Windows 11 build 26300, on a development build between v1.0.3 and v1.0.4. This is a
different CPU from round 2's, so no figure here is compared with round 2's. Each timed process ran
on one logical CPU with the processor's clock capped at 5,200 MHz, and every cost judged at one
batch size was timed in two sessions and read in both. Every miss below was predicted, with its
cause, before the round was timed, and each came in within its predicted bound.

| Budget | Result | Measured |
|---|---|---|
| CPU, per operation | **missed** in 1 of 9 workloads, as predicted | a render pass: 68.56 and 70.22 ns above native in the two sessions against a budget of 35.00 (predicted at most 90), in the scope's bookkeeping, the starting state every pass sets, the core's work for a barrier the backend then leaves out, and a part that moves with where the executable is loaded, whose mechanism is not yet known. Every other workload meets in both sessions, a pipeline switch and draw at 7.33 and 8.22 ns against 10.00, a counted indirect draw at 4.33 and 4.17 against 5.00, and a 256-byte upload at 0.00 against 5.00, now that the native code writes its frame data to the same GPU upload heap as RojoRHI. The submission meets, judged at 100 a list by its per-call floor: 89.8 and −38.5 ns against 678.6 and 687.5 |
| CPU, fixed cost per command list | **missed** in 2 of 9 workloads, as predicted | a draw with a root block, 592.3 and 493.6 ns against a budget of 414.2 and 414.3 (predicted at most 970), in the core's list begin, the root constant `drawId` needs and the frame arena's first claim; a render pass, 529.9 and 624.4 ns against 398.6 and 398.4 (at most 1,200), in the pass's scope and the pass itself. A dispatch with a root block is not settled: 294.2 ns against 360.8 in one session, within its budget, and 392.2 against 360.5 in the other. Every other workload meets in both sessions. The two sessions of a fixed cost may differ by up to the registered spread, 351 ns, about the size of the budgets themselves, so on this machine a fixed cost's result says little more than which side of its budget each session fell |
| GPU time, composed frame | met | 741.09 µs against 740.85 µs; the paired difference is +74 ns [−122, 382] against a budget of 7.41 µs, +0.01% |
| Heap allocations per frame | met | counted as C++ `operator new` calls, not the Metal measure: 0 against 0 in every configuration; RojoRHI's own allocations are 0 in every frame |
| Native calls | met | equal to native per repetition in every judged workload |
| Pipelines and native objects after warm-up | met | none in any configuration |

Two GPU figures outside the composed frame are reported, not judged, and the cause of each was found
after the round: a dispatch and barrier is 5.49% to 5.81% above native at every batch size from 100
a list, and a dispatch with a root block at 100,000 a list is 303.62 µs a frame above native
[297.98, 354.56], +5.32%. The first comes from RojoRHI's shader-read barrier, which also names constant-buffer access on a global barrier, about 0.083 µs a barrier on this GPU and driver; 1.0.4 keeps it, and the next minor release drops it. The second was found in that configuration alone: the gap follows the GPU's idle time before each frame, which is longer for RojoRHI because it spends 0.66 ms more recording the frame, and with the GPU's clock held it is 0.11%. Nothing is claimed for frames that keep the GPU busy while recording.

Work on the CPU cost of both backends continues, and each backend's next round replaces its table.
