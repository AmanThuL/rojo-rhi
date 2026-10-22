# The Metal 4 backend's overhead against native code

**Status**: Accepted (2026-10-18)

This note is the T2 benchmark round for the Metal 4 backend: the measurement suite of
[ADR 0014](../decisions/0014-overhead-measurement.md), run against hand-written metal-cpp code, to
the budgets that ADR fixes. It is the round's pre-registration: the workloads, metrics, statistics
and decision rules below are fixed before any measurement runs, and changing them invalidates the
round. The results section is filled in when the round reports; nothing above it changes
afterwards. A later round is a new note.

## Question

Does RojoRHI on Metal 4 stay within ADR 0014's budgets against the best hand-written Metal 4 code
for the same workloads?

| Metric | Budget (ADR 0014) | Judged here as |
|---|---|---|
| CPU tax per operation over native | at most 5% | C1 to C10 at `K` of 100 and above |
| GPU frame-time tax over native, composed frame | at most 1% | the composed frame only |
| Hidden pipeline variants | zero | no pipeline created after warm-up |
| Per-frame heap allocations after warm-up | zero | R allocates no more per frame than N, since Metal's own allocations appear in both |
| Native calls per RojoRHI call on hot paths | at most the native minimum plus one | an operation of `c` RojoRHI calls makes at most N's count plus `c` |

## Arms

| Arm | What runs |
|---|---|
| N, native | metal-cpp code written for the workload: the fewest native calls that do its work, with the same resources, residency and frame pacing a careful renderer uses, and the same shaders as R: the MSL that Slang emits for the same Slang sources, except G1 and G2, whose N kernels are hand-written MSL |
| R, RojoRHI | the same workload through `RHI.h` on the `metal4` backend, in a Release build with link-time optimization, shaders compiled from Slang through the module, and a fixed `DeviceDesc`: `framesInFlight` 3, `validation` false, `enableCapture` false, an empty `pipelineCache` |

Both arms live in one benchmark executable, so a process measures both on the same device state.
Both run three frames in flight and, apart from G2's arm F, use the ID table for texture indexing,
the lowering spike 2 chose. N follows the mappings of the API reference where they are the native
minimum and departs from them where native code would do less; each workload states its departures,
`none` when it has none.

## Workloads

**CPU operations.** Each workload records one command list per frame holding `K` repetitions of one
operation, inside the passes and encoders the operation needs: C1 to C3 and C8 render to a 256×256
`RGBA8Unorm` target; C1 to C3 set the viewport and scissor, and C1 and C2 the pipeline, once before
the repetitions; C4 and C6 set their compute pipeline once. `K` is swept over 1, 10, 100, 1,000,
10,000 and 100,000, except where a workload states otherwise. `c` is the number of RojoRHI calls per
repetition, and N's column lists its native calls per repetition.

| Id | Operation per repetition | `c` | N per repetition | Departures |
|---|---|---|---|---|
| C1 | `pushRoot` of a 64-byte block (`uploadFrameData`, `setRoot`), then `draw` of 3 vertices | 3 | arena copy, `setAddress` on the argument table, `drawPrimitives`: 2 calls | none |
| C2 | `pushRoot`, then `drawIndexed` of 3 `UInt16` indices | 3 | as C1 with `drawIndexedPrimitives`: 2 calls | none |
| C3 | `setPipeline` alternating between two pipelines that differ in cull mode and depth-stencil state, then `draw` | 2 | `setRenderPipelineState`, `setCullMode`, `setDepthStencilState`, `drawPrimitives`: 4 calls | none |
| C4 | `pushRoot`, then `dispatch` of one group | 3 | arena copy, `setAddress`, `dispatchThreadgroups`: 2 calls | none |
| C5 | `uploadFrameData` of 256 bytes | 1 | arena copy: 0 calls | none |
| C6 | `dispatch`, then a global barrier from `ComputeShader`/`ShaderWrite` to `ComputeShader`/`ShaderRead` | 2 | `dispatchThreadgroups`, `barrierAfterEncoderStages`: 2 calls | none |
| C7 | `copyBuffer` of 256 bytes between `Gpu` buffers | 1 | `copyFromBuffer`: 1 call | none |
| C8 | `barrier` on the target (none before the first pass), `beginRenderPass` with one color attachment, `setPipeline`, `setViewport`, `setScissor`, one `draw` of 3 vertices, `endRenderPass` | 7 | the 12 calls listed below | N binds no argument table, since the pass's shaders read no reserved binding; R binds the list's table at every encoder it opens (ADR 0018) |
| C9 | `Queue::submit` of one pre-recorded list, signalling one fence value | 1 | `commit`, `signalEvent`: 2 calls | none |
| C10 | counted `drawIndirect` with `maxCount` 256 and a count of 256 | 1 | spike 3's arm E, encoded by hand: the kernels of the pass batched in one compute encoder ahead of the render encoder, as R batches them | none |

C8's barrier exists because consecutive passes write the same target, which the validation tracker
rejects without one (`VU-rojo-sync-0021`). After each `endRenderPass` but the last, R records
`barrier` with one `TextureBarrier` on the target: `ColorAttachment`/`ColorAttachmentWrite` →
`ColorAttachment`/`ColorAttachmentRead|ColorAttachmentWrite`, with the old and new layout both
`ColorAttachment`. Its source lies in an earlier encoder, so chapter 09 lowers it to a consumer
`barrierAfterQueueStages` on the next render encoder, from the fragment stage to the fragment stage
with visibility `Device`; N encodes that call by hand.

C8's sequence, in order. R makes 13 native calls per repetition: on the command buffer
`pushDebugGroup` and `writeTimestampIntoHeap` at `beginRenderPass`; at the draw, the encoder opens
with `renderCommandEncoder`, `setLabel` and `setArgumentTable`, then the consumer
`barrierAfterQueueStages` (none in the first pass), the cached `setRenderPipelineState`,
`setViewport` and `setScissorRect`, then `drawPrimitives`; at `endRenderPass`, `endEncoding`, then
`writeTimestampIntoHeap` and `popDebugGroup` on the command buffer; `barrier` itself makes no call.
N makes the same calls in the same order except `setArgumentTable`, 12 in all. N mirrors the
profiling markers R's pass scope emits (the debug group, the two timestamps and the encoder label),
because a renderer with GPU profiling writes them. C8 runs `K` up to 10,000 only: one Metal 4
command buffer aborts the process between 95,000 and 100,000 render encoders, and two timestamps
per pass at 100,000 passes exceed the 32 counter heaps Metal allows a process.

C9 records `K` lists before the timed span, `K` in 1, 10 and 100, each holding one `copyBuffer` of
256 bytes, then makes `K` submit calls of one list each, each signalling the next value of one
fence; only the submit calls are timed. C10 draws spike 3's workload, one point per record into a
256×256 `R32Float` target, and runs `K` up to 1,000 only, because its indirect command buffers take
about 170 KiB each. C10 also runs at `maxCount` 1 with a null count, as a report-only row: records
keep their layout (#31, decided in the backend design), and this row reports what that costs Metal.
C1 to C9 at `K` above 1 are the scaling sweeps of ADR 0014's principle 4 for draw count (C1, C2),
binding churn (C3), barrier density (C6) and pass count (C8).

**GPU kernels.** Report-only: the reference already states the ID table's cost from spike 2. A
compute kernel reads data through the shader module, against the same kernel in hand-written MSL,
at 2^20 threads, with indices uniform across a SIMD group and divergent per thread. The loads are
integer loads and point-sampled texels of exact values, so both arms' outputs compare exactly.

| Id | Kernel | N | Departures |
|---|---|---|---|
| G1 | sums 16 `uint4` loads through an `Addr` from the root | the same loads through a device pointer in the root | none |
| G2 | sums 16 point samples of 128 `R32Float` 16×16 textures holding integers below 2^16, through `ViewIndex` and `SamplerIndex` from the root | arm T reads the ID tables the same way; arm F, spike 2's fixed slots, binds the 128 textures in argument-table slots with a constant nearest sampler, for the record | F uses argument-table texture slots, which the reference does not |

**Composed frame.** A fixed mini-renderer of 14 passes over a procedural scene of 10,000 boxes in
64 meshes, at 1920×1080, written twice: through RojoRHI and natively. The scene, camera and three
shadow cascades are fixed functions of the box index, the same every frame. Resources:

- `instances`, a `Gpu` buffer of 10,000 64-byte transforms; `vertices` and `indices`, the 64 box
  meshes (24 vertices and 36 `UInt16` indices each, box `i` using mesh `i mod 64`).
- `records[s]`, one `Gpu` buffer per frame slot `s` of 3: four views (cascades 0 to 2, then the
  camera) of 10,000 indexed records and a count each.
- `shadow`, a `D32Float` 2048×2048 array of 3 layers; `albedo` and `material` in `RGBA8Unorm`,
  `normal` in `RGBA16Float`, `depth` in `D32Float`, `aoRaw` and `ao` in `R8Unorm`, `hdr` and `ldr`
  in `RGBA16Float` and `RGBA8Unorm`, all 1920×1080; `bloom0` and `bloom1` in `RGBA16Float` at
  960×540 and 480×270; `readback[s]`, a `Readback` buffer of 7,680 bytes per slot.

The culling keeps a fixed 60% subset by index: box `i` is kept in every view when `i mod 5 < 3`, its
record written at index `3⌊i/5⌋ + i mod 5` with `drawId` `i`, so each count is 6,000 and the order
is deterministic. Every counted draw has `maxCount` 10,000 and comes first in its pass, the common
shape, in which the backend hoists their encoding kernels ahead of the pass; a pass split is not
measured. Depth is reverse Z, cleared to 0 and tested with `Greater`. A barrier below reads
`resource: old→new layout, srcStage/srcAccess → dstStage/dstAccess`.

| # | Pass (label) | Queue, list | Work | Inputs | Outputs | Barrier batch before it |
|---|---|---|---|---|---|---|
| 1 | `cull` | Compute, `cull` | `dispatch(625, 1, 1)`, 64 threads a group, one per box and view | `instances` | `records[s]` | none |
| 2 | `geometry` | Graphics, `gbuffer` | one counted `drawIndexedIndirect`, camera view; clears all targets | `records[s]`, `instances`, meshes | `albedo`, `normal`, `material`, `depth` | `albedo`, `normal`, `material`: Undefined→ColorAttachment, ComputeShader/None → ColorAttachment/ColorAttachmentWrite; `depth`: Undefined→DepthStencilWrite, DepthStencil/None → DepthStencil/DepthStencilRead+DepthStencilWrite |
| 3 | `ao` | Compute, `ao` | `dispatch(240, 135, 1)` writing `aoRaw`, a barrier `aoRaw`: ShaderWrite→ShaderRead, ComputeShader/ShaderWrite → ComputeShader/ShaderRead, then a blur `dispatch(240, 135, 1)`; 8×8 groups | `depth`, `normal` | `ao` | `aoRaw`, `ao`: Undefined→ShaderWrite, None/None → ComputeShader/ShaderWrite |
| 4–6 | `shadow0` to `shadow2` | Graphics, `shadows` | one counted `drawIndexedIndirect` each, cascade view, into its layer; clears it | `records[s]`, `instances`, meshes | `shadow` layer 0 to 2 | before pass 4 only: `shadow`: Undefined→DepthStencilWrite, ComputeShader/None → DepthStencil/DepthStencilRead+DepthStencilWrite |
| 7 | `lighting` | Graphics, `lighting` | `dispatch(240, 135, 1)`, 8×8 groups | `albedo`, `normal`, `material`, `depth`, `shadow`, `ao` | `hdr` | `shadow`: DepthStencilWrite→ShaderRead, DepthStencil/DepthStencilWrite → ComputeShader/ShaderRead; `hdr`: Undefined→ShaderWrite, FragmentShader/None → ComputeShader/ShaderWrite |
| 8 | `forward` | Graphics, `lighting` | 500 alpha-blended boxes in index order, each a `pushRoot` of its 64-byte transform and color and a direct `drawIndexed`; `depth` loaded, tested, not written | `depth`, meshes | `hdr` | `hdr`: ShaderWrite→ColorAttachment, ComputeShader/ShaderWrite → ColorAttachment/ColorAttachmentRead+ColorAttachmentWrite; `depth`: ShaderRead→DepthStencilWrite, ComputeShader/None → DepthStencil/DepthStencilRead |
| 9 | `bloomDown0` | Graphics, `lighting` | one fullscreen `draw` of 3 vertices, linear sampling | `hdr` | `bloom0` | `hdr`: ColorAttachment→ShaderRead, ColorAttachment/ColorAttachmentWrite → FragmentShader/ShaderRead; `bloom0`: Undefined→ColorAttachment, FragmentShader/None → ColorAttachment/ColorAttachmentWrite |
| 10 | `bloomDown1` | Graphics, `lighting` | as pass 9 | `bloom0` | `bloom1` | `bloom0`: ColorAttachment→ShaderRead, ColorAttachment/ColorAttachmentWrite → FragmentShader/ShaderRead; `bloom1`: Undefined→ColorAttachment, FragmentShader/None → ColorAttachment/ColorAttachmentWrite |
| 11 | `bloomUp1` | Graphics, `lighting` | one fullscreen `draw`, additive blend, target loaded | `bloom1` | `bloom0` | `bloom1`: ColorAttachment→ShaderRead, ColorAttachment/ColorAttachmentWrite → FragmentShader/ShaderRead; `bloom0`: ShaderRead→ColorAttachment, FragmentShader/None → ColorAttachment/ColorAttachmentRead+ColorAttachmentWrite |
| 12 | `bloomUp0` | Graphics, `lighting` | as pass 11 | `bloom0` | `hdr` | `bloom0`: ColorAttachment→ShaderRead, ColorAttachment/ColorAttachmentWrite → FragmentShader/ShaderRead; `hdr`: ShaderRead→ColorAttachment, FragmentShader/None → ColorAttachment/ColorAttachmentRead+ColorAttachmentWrite |
| 13 | `toneMap` | Graphics, `lighting` | one fullscreen `draw` | `hdr` | `ldr` | `hdr`: ColorAttachment→ShaderRead, ColorAttachment/ColorAttachmentWrite → FragmentShader/ShaderRead; `ldr`: Undefined→ColorAttachment, Copy/None → ColorAttachment/ColorAttachmentWrite |
| 14 | `readback` | Graphics, `lighting` | `copyTextureToBuffer` of row 540 | `ldr` | `readback[s]` | `ldr`: ColorAttachment→CopySrc, ColorAttachment/ColorAttachmentWrite → Copy/CopyRead |

The `gbuffer` list ends with one more batch after pass 2, before the handoff: `albedo`, `normal`,
`material`: ColorAttachment→ShaderRead, ColorAttachment/ColorAttachmentWrite →
ComputeShader/ShaderRead; `depth`: DepthStencilWrite→ShaderRead, DepthStencil/DepthStencilWrite →
ComputeShader/ShaderRead. The `ao` list ends with `ao`: ShaderWrite→ShaderRead,
ComputeShader/ShaderWrite → ComputeShader/ShaderRead. A frame `f`, counted from 1, makes five
submissions in this order, with three fences each signalled once a frame with the value `f`:

| Submission | Queue | Lists | Waits | Signals |
|---|---|---|---|---|
| 1 | Compute | `cull` | none | `cullDone` = `f` |
| 2 | Graphics | `gbuffer` | `cullDone` = `f` | `gbufferDone` = `f` |
| 3 | Compute | `ao` | `gbufferDone` = `f` | `aoDone` = `f` |
| 4 | Graphics | `shadows` | none | none |
| 5 | Graphics | `lighting` | `aoDone` = `f` | none |

So the occlusion overlaps the shadow passes, and the cascades read `records[s]` behind the wait of
submission 2. Both arms label each list and pass with the short fixed label above, and both write a
debug group and a pair of timestamps per pass: R through the scope each render pass forms and a
`beginScope`/`endScope` around passes 1, 3, 7 and 14. R's barriers are exactly the batches listed,
which the validation tracker accepts with no violation; N emits the native stage barriers that
chapter 09's lowering makes of them, in the same forms (a batch with no source stage makes none).
Rendering is offscreen with no presentation, because the harness has no window, so the frame omits
the drawable wait and present that a real frame pays. Departures: none.

## Metrics

1. **Correctness gate.** For every workload and configuration, R's output equals N's exactly: the
   render targets and buffers each workload writes are read back and compared byte for byte. Both
   arms run the same shaders, so the composed frame and C1 to C10 compare exactly; G1 and G2 do
   because their values are exact. R also runs once in Debug with `DeviceDesc::validation` and
   `MTL_DEBUG_LAYER=1` and N with `MTL_DEBUG_LAYER=1`; each reports no error. A workload failing
   the gate is not timed.
2. **Native calls.** A native call is a call on a Metal device, queue, command buffer, encoder,
   argument table, residency set, event, heap or counter heap; property sets on descriptors and on
   `MTL4::CommitOptions` count in neither arm. R's count comes from `Device::counters`
   (`nativeRecordCalls`, `nativeSubmitCalls`, `nativeHostCalls`, `nativeBarriers`), which the
   backend increments at every native call site; N's is counted by hand at each call site. The
   count per operation is the difference of two `K` values: `(count at K = 1,000 - count at K =
   100) / 900`, and for C9 `(count at K = 100 - count at K = 10) / 90`. Exact.
3. **Allocations.** Every allocation through the default malloc zone, counted by a wrapper the
   benchmark executable installs over that zone's functions, during the measured frames of R and
   of N. A process's value is the median over its measured frames; R's `cpuAllocations` is
   reported beside it.
4. **Pipelines.** `pipelinesCreated` and `nativeCreateCalls` after warm-up. Exact.
5. **CPU time.** Host clock, of 41.7 ns resolution, from list acquisition to `end` (N:
   `beginCommandBuffer` to `endCommandBuffer`), so lazy encoder opens are inside the span; for C9,
   the submit calls only. Per operation, the span divided by `K`.
6. **GPU time.** Commit feedback: R through the backend's test-only seam, which attaches
   `MTL4::CommitOptions` feedback to every commit, and N attaching the same feedback to its own
   commits, in every run of both arms. The composed frame's GPU time is the span from the earliest
   `GPUStartTime` of its five commits to the latest `GPUEndTime`, across both queues; G1 and G2
   use their single commit's span. No time is summed over command buffers.

## Statistics

- Release builds with link-time optimization; validation off for timed runs and on for the
  correctness runs.
- Each repetition is one fresh process for one workload and `K` that runs both arms, R first in
  even processes and N first in odd ones. 20 processes per configuration, interleaved across the
  configurations of a workload, on mains power.
- Per process and arm: 50 warm-up frames, then 200 measured frames, each committed and waited to
  completion; the process median is kept, and the reported value is the median of process medians.
- Paired deltas R minus N within a process; two-sided 95% percentile bootstrap confidence
  intervals with 10,000 resamples over processes, seed 20261018.
- Recorded per run: device and GPU family, macOS build, Xcode and `metal` versions, the metal-cpp
  pin, the repository commit, power source, and the thermal state at start and end. A process that
  starts or ends above the nominal thermal state is discarded, followed by 120 s of rest, and rerun,
  at most 4 attempts in all; a process that fails its fourth attempt reports its configuration as
  not run.

## Decision rules

- **CPU.** A configuration at `K` of 100 or above misses the CPU budget when the interval of R
  minus N excludes zero and the median delta exceeds 5% of N's median; smaller `K` are reported.
  C10 is judged on CPU time against arm E; the reference already marks counted draws `costly` for
  their native-call count, so its call count is reported and not judged. C10 at `maxCount` 1 is
  reported only.
- **GPU.** The composed frame misses the GPU budget when the interval of its frame-time delta
  excludes zero and the median delta exceeds 1% of N's. G1 and G2 are reported only.
- **Counts.** Any operation of C1 to C9 whose native calls exceed N's count plus its `c`, any
  workload whose R allocations per frame, as the median of process values, exceed N's, or any
  pipeline created after warm-up, misses the budget.
- **A miss** is a defect in the backend: the backend is fixed and the round is run again as a new
  note. Texture indexing is reopened (issue #30) only when the composed frame misses its GPU budget
  and investigation shows that texture indexing is the cause.
- The results change the API reference in the same change that records them where a mapping's
  stated cost was wrong.

## Procedure

- The harness is `Benchmarks/`, built by xmake as `RojoRHIBench` on the `metal4` backend, with a
  driver, `Tools/bench.py`, that builds, runs the correctness gate, runs the timed processes,
  resumes an interrupted run and analyses the results, as the spikes' drivers did.
- On hosted CI every workload's R arm runs on the Null backend, which asserts Null's counts for
  each mapping row and the workload's sequence of RojoRHI calls, so a change to either fails before
  any timing. A local `[metal4]` counter test runs C1 to C9 at the two `K` of metric 2 and asserts
  R's per-operation native-call counts against N's count plus `c`.
- Raw logs and per-process results are kept in `rojo-rhi-evidence/t2/<date>/` with checksums
  ([GPU evidence](../guides/gpu-evidence.md)); this note keeps the environment, the summary tables
  and the decisions.

## Amendments

2026-10-18, before any timed run: C8 registers the barrier between consecutive passes, so its `c`
becomes 7, N's count 12 and R's 13; and C8's `K` sweep stops at 10,000, for the two Metal limits
stated with its sequence. Both were found while implementing the harness, before anything was timed.

2026-10-18, before any timed run: the barriers of rows 2 (`geometry`, `depth`) and 4
(`shadow0`, `shadow` before pass 4) now end at `DepthStencilRead+DepthStencilWrite`, because those
passes test and write depth, which chapter 09 counts as both accesses; stages and layouts are
unchanged. The change adds no native call, since Metal 4 barriers are stage-based.

2026-10-19, before any timed run, after checking every registered sequence and workload against
the backend and the harness. Nothing here changes a budget, a workload, a `K` or a rule:

- C8: R makes 14 native calls per repetition (13 in the first pass), the registered sequence with
  `setFrontFacingWinding` between `setRenderPipelineState` and `setViewport`: the backend sets the
  winding once per encoder it opens for a pipeline that culls, tests stencil or has a fragment
  entry. The budget stays N's 12 plus `c` of 7. R takes its timestamps from heaps the device owns
  per frame slot, at most 8 of 4,096 entries, so at `K` 10,000 each arm holds 15 heaps and the two
  arms of a process never hold them at once; the sweep limit stands for N's heaps and the encoder
  abort, and R would leave passes past 16,384 a frame untimed rather than exceed the limit.
- C9: R makes 3 native calls per submit, the `commit`, the fence's `signalEvent` and a
  `signalEvent` of the queue's progress event, against N's 2 and the budget of 3.
- R departures that cost CPU time and no counted call: an autorelease pool around every native
  draw, dispatch, copy and fill, around each encoder's open and end and around each timestamp
  written on a command buffer (issue #60); a label string made per list and per pass; the pass
  descriptor configured for each encoder; a label on every command buffer and encoder. Every render
  pass of R is a timed scope, so C1 to C3 and C10 pay a debug group and two timestamps per frame
  that their N arms, as registered, do not write.
- R departures per frame, which change no per-operation count: C6 splits its compute encoder at the
  fill's barrier and closes the list with a trailing encoder for the last held barrier; C10 closes
  with a trailing encoder for the stages its target barrier leaves pending; in the composed frame R
  closes the `gbuffer`, `ao` and `lighting` lists with trailing encoders, splits the `ao` encoder at
  its closing batch, lowers the readback copy to `Blit | Dispatch | Fragment`, binds residency and
  sets the winding per encoder, and signals the queue's progress and frame events. The untimed
  gate counted 1,293 native calls a frame for R and 1,254 for N.
- N departures from the reference's mappings, where native code does less: C6's global barrier
  makes its visibility `Device`, where chapter 09 lowers it with `Device | ResourceAlias`, since
  nothing in C6 aliases; the encoding kernels of the counted draws in C10 and the composed frame are
  hand-written MSL copies of the backend's kernel, not Slang output, and N sizes its indirect
  command buffers to `maxCount`; C10's target, records and count are `Shared` in N and `Gpu` in R.
  C10 is judged on CPU time, so none of these reaches a judged GPU number. In G1 and G2, both
  report-only, R writes its output to one of three regions by frame slot and N rewrites one.
- Instrumentation: the commit-timing seam passes Metal a block, as N does, instead of metal-cpp's
  `std::function` overload, whose wrapper and `__block` copy made about three allocations per commit
  in R only; and N's handler opens the same autorelease pool as the seam's. A timed process that
  prints any line on standard error stops the round, so a refused counter heap, which leaves a scope
  untimed and R with less work, cannot pass for a faster R.
- Analysis, as the harness has implemented it since the note was accepted: the allocation rule is
  applied per configuration, so a workload misses when any of its configurations does; and a count
  that differs between frames or processes is reported unstable and judged on its maximum.

2026-10-20, before any round-2 timed run: round 2 is registered by amendment, as [round 2](
2026-10-20-metal4-overhead-round2.md) states; it changes no budget, workload, `K` or rule.

2026-10-21, before any round-3 timed run: round 3 is registered by amendment, as [round 3](
2026-10-21-metal4-overhead-round3.md) states. It changes two rules after results were seen, the CPU
budget (ADR 0025) and the allocations of C10 at `maxCount` 1, and also N's frame completion and R's
`DeviceDesc`.

2026-10-22, before any round-4 timed run: round 4 is registered by amendment, as [round 4](
2026-10-22-metal4-overhead-round4.md) states; it keeps every rule of round 3 and changes no budget,
workload, `K` or rule.

## Results

Run on 2026-10-19 at 4f2e0d6: 1,260 processes, all nominal on their first attempt. Every row, the
cause analysis and the decisions are in [the results](2026-10-18-metal4-overhead-results.md). The
round misses three budgets: CPU per operation (33 of 34 judged configurations; C1 to C7 by 23% to
68%), the composed frame's GPU time (+85.6 µs [82.7, 89.6], +4.37%) and allocations (R above N in 61
of 63 configurations). Hidden pipeline variants (none) and native calls (all within N plus `c`, C9
with no margin) are met. Texture indexing is not the cause of the GPU miss; #30 is not reopened.

Round 2 ran on 2026-10-20 at 0a0b53f ([results](2026-10-20-metal4-overhead-round2-results.md)): it
meets GPU (−2.40%), pipelines and native calls; misses CPU (15 of 34) and allocations (57 of 63).

Round 3 ran on 2026-10-21 at 378f219a ([results](2026-10-21-metal4-overhead-round3-results.md)),
judged by ADR 0025: it meets GPU (−2.20%), allocations, pipelines and native calls, and misses CPU
in 1 of 34, C3-k100 (13.96 ns against 10); the 5% rule, reported, misses C3 at every judged `K`.
