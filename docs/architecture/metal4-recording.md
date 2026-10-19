# Command Recording on the Metal 4 Backend

**Status**: Accepted

This page describes how the Metal 4 backend records a command list into native command buffers:
the objects a list owns, the one open encoder, copies, fills, clears, barriers, scopes, the counts
and the commit-timing seam. [The Metal 4 backend](metal4-backend.md) describes the device around
it, and [drawing](metal4-drawing.md) the render passes, pipelines, dispatches and draws a list
also records. Timestamps are not recorded yet. Chapters 07, 09 and
10 of the API reference stay the authority on semantics and on each entry's Metal 4 mapping; every
check lives in `Source/Core`, so no `ROJORHI_VU` appears under `Backends/Metal4/`.

## What a list owns

A `CommandListImpl` owns, from its creation when its frame slot's pool grows, a `ListState`: an
`MTL4::CommandAllocator`, an `MTL4::CommandBuffer`, an `MTL4::ArgumentTable` and a second table for
backend kernels. The list table has four buffer bindings with `buffer(1)` and `buffer(2)` holding
the view and sampler tables' GPU addresses from the start (ADR 0018); the kernel table has two
buffer bindings and one texture binding. Each creation counts one `nativeCreateCalls`, so a new list
counts four, and the list counts one `cpuAllocations`.

`acquireCommandList` runs the core's `beginList`, then `beginCommandBuffer(allocator)` and
`setLabel` with an `NS::String` made by `alloc()->init` from the resolved label. The list keeps the
string for its encoders' labels and releases it at the next acquisition. `end` runs the core's
`endList`, closes the open encoder, emits a barrier still owed (below) and calls `endCommandBuffer`.
`Queue::submit` commits every list's command buffer in one `commit(buffers, count)` between the
waits and the signals, from a vector the queue reuses, and a submission with no lists commits
nothing. When a frame slot is reused, the `frameSlot` hook resets the allocator of every list the
slot took, one `nativeHostCalls` each, before returning them to the pool. `destroyDevice` releases
every list's objects, in reverse order of creation inside a pool, after the queues drain; a drain
ends with an empty commit that waits for its feedback handler, as the stager does, so a list's
allocator is released for the last time on the destroying thread and not on Metal's completion
thread, which holds no autorelease pool.

## The open encoder

A list has at most one open encoder (`metal4::Encoders`). The compute encoder opens lazily at the
first copy, fill or kernel: the factory runs inside an `AutoreleasePool` and the encoder is
retained; then `setLabel` (the innermost open scope's label, else the list's), `setArgumentTable`
with the list table, and the part of the pending barrier that falls on its stages. It closes at a
scope boundary, before a clear's render encoders, at a barrier that must order earlier work, and at
`end`. The list records whether the open encoder holds work. A clear's render encoders and the
trailing encoder of the barrier lowering bind no table, since no shader runs on them.

Recording reads native objects only through `HandlePool::native` and the list's own state, never a
pool record, which is read under a lock. A texture's type, pixel format, usage, mip and layer counts
and extents come from the native texture's getters, and a format class comes from the
`MTL::PixelFormat` through a backend switch (`formatOf`). Per-command paths open no pool and call no
autoreleasing factory, except where the next paragraph says.

The Metal validation layer and the driver autorelease on some native calls: the layer on every
dispatch it checks, the driver the first time it builds an internal blit pipeline for a copy shape.
The copy entries, the native `fillBuffer`, every backend kernel dispatch, dispatch and draw
therefore open an `AutoreleasePool` around their native call, as does the render path of a clear
and the end of a render encoder. Barriers and debug
groups open none. This is a runtime effect of the layer, so Release builds pay it too.

## Copies

Chapter 07 part 3 on the compute encoder:

- buffer to buffer is one `copyFromBuffer`;
- buffer to texture and texture to buffer are one call per array layer (cube faces are layers), or
  one call over the depth for a 3D texture; `bytesPerImage` is `rowPitch` times `rowsPerImage` (or
  the region's height in blocks) for 3D, array and cube types and 0 for 1D and 2D;
- texture to texture is one `copyFromTexture` per layer, or one over the depth when either texture
  is 3D, and its native size counts whole blocks, which Metal clamps at a mip's edge;
- formats that differ only in sRGB copy directly, and the validation layer accepts that.

An equal-byte `fillBuffer` is one native `fillBuffer`. Any other pattern runs the fill kernel.

## Backend kernels

Kernels are MSL files under `Backends/Metal4/Kernels/`. The rule `rojorhi.metal4_kernels` in
`xmake/kernels.lua`, applied to the `RojoRHI` target on `metal4` only, compiles each file with
`xcrun metal` and links them with `xcrun metallib` into `build/rojorhi/Metal4Kernels/`, then writes
the bytes to the generated header `build/rojorhi/Metal4KernelLibrary.h`, which is never committed.
A build needs the Metal toolchain on the machine (`xcrun metal --version`; install it with
`xcodebuild -downloadComponent MetalToolchain`, which the `metal4` CI job does); a Null build never
compiles a kernel. A step is skipped while its output is newer than its inputs.

`createDevice` loads the embedded library with `newLibrary` from `dispatch_data_t` and makes every
kernel pipeline with `MTL4::Compiler::newComputePipelineState` over an
`MTL4::LibraryFunctionDescriptor`, wrapped in an `MTL4::SpecializedFunctionDescriptor` where
function constants select a variant. Neither the objects nor the calls are counted, as for the
device's other objects. A failure is a `DriverFailure` naming the library or the kernel, and
`destroyDevice` releases the pipelines, then the library.

A kernel run is `bindKernel` (set the pipeline unless the open encoder already has it, then the
kernel table), the caller's bindings, and `dispatchKernel` (dispatch inside a pool, then set the
list's table again). `Encoders::kernelPipeline` remembers the encoder's pipeline, whether a kernel's
or the caller's compute pipeline, and is reset when an encoder opens or closes. A kernel leaves its
own pipeline on the encoder, and the next `dispatch` sets the caller's again when it differs
([drawing](metal4-drawing.md#compute-binding-and-dispatches)). The counted-draw kernels of a render
pass run on a compute encoder that holds only kernels, through the batch form (`bindKernelBatch`,
`dispatchKernelBatch`), which sets the kernel table once and does not set the list's table back
([counted draws](metal4-drawing.md#counted-draws)). Parameters come from the frame arena through
`core::bumpArenaPage`, which locks only to claim a page, and count as no frame data upload.

The fill kernel writes one 32-bit word per thread, 64 threads per group, at most 2^31 words per
dispatch. The pattern's bytes land in little-endian order.

## Clears

`clearTexture` clears every subresource of its range, resolved from the native texture's getters.
A texture whose native usage has `RenderTarget` (a render target or depth-stencil texture) clears
by load actions: for each mip and layer, and each depth plane of a 3D texture, one render encoder
from an `MTL4::RenderPassDescriptor` with a clear load action and a store action, the clear colour,
the clear depth and, for a stencil format, the clear stencil. The compute encoder closes first, the
render encoder emits only the fragment part of a pending barrier when it opens, where its load and
store actions run, and keeps the vertex, object and mesh stages pending. It closes straight after,
so each subresource is one `encodersOpened`.

A `Storage` texture without `RenderTarget` clears by kernel. `Format` has no integer format, so
every storage format writes a `float4`. A few functions, specialised by function constants into one
pipeline per Metal texture type a `Storage` texture can have, run one dispatch per mip over its
layers or depth, in groups of 8 by 8, with writes guarded against the mip's extent. A texture with
both usages takes the render path. A `Compute` list only ever clears `Storage`-only textures.

## Barriers

Chapter 09's table lowers stages to Metal stages, with `Copy` as `StageBlit | StageDispatch |
StageFragment` and `All` as every stage of the queue's encoders. Both sides of a batch are masked
by the queue: graphics lists keep compute and render stages, compute lists `StageDispatch |
StageBlit`, and copy lists `StageBlit`, since those queues never run another encoder
(`VU-rojo-sync-0001`). Visibility is `VisibilityOptionDevice` when any access is not `None`, with
`ResourceAlias` added for a global barrier and for a texture barrier from `Undefined` on a placed
texture (the native texture is `Untracked`, which only placed textures are). A batch with an empty
source or destination side emits nothing.

The list keeps a pending barrier (source stages P, destination stages D still owed, visibility V)
and, for the open encoder, what the queue-level barrier at its opening covered (sources `S_open`,
destinations `D_open`). A batch (T, U, V') recorded while the open encoder holds work, with T inside
`S_open` and the encoder's part of U inside `D_open`, has the encoder-level form: it is held back
and emitted as `barrierAfterEncoderStages` just before the encoder's next command in a destination
stage, and U outside the encoder joins the pending barrier. If the encoder closes first, the held
barrier joins the pending one. Any other batch closes an open encoder that holds work and joins the
pending barrier. A compute encoder that opens with D inside its stages first emits
`barrierAfterQueueStages(P, D, V)`, records what that covered and removes those stages from D; a
clear's render encoder emits only the fragment part of D and leaves the other render stages owed. A
stage the opening barrier held back but the encoder ran no command in returns to the pending barrier
when it closes.

The condition is stricter than "work since the last queue-level barrier" because a source stage
that the opening barrier did not cover may name work of an earlier list, which an encoder-level
barrier cannot order. The first barrier after a list's first encoder opens is therefore
queue-level, and repeated barriers settle on the encoder-level form.

A barrier still pending at `end` is emitted on a closing compute encoder that holds no command:
`barrierAfterQueueStages(P, Dispatch | Blit, V)`, `barrierAfterStages(Dispatch | Blit, D, V)`,
`endEncoding`. One encoder, two barriers; without it a barrier at the end of a list would order
nothing in later lists. It rests on one assumption of the lowering, which a conformance case is
built to falsify: Metal runs each stage of successive encoders on a queue in order, so a consumer
barrier on one encoder also holds back the same stages of every encoder after it.

Worked sequence on a graphics list: copy A, barrier (copy to copy), copy B, barrier, `end`.

| Step | Native result |
|---|---|
| copy A | the compute encoder opens (no pending barrier, none emitted); `copyFromBuffer` |
| barrier 1 | its sources are not covered by an opening barrier that emitted none: the encoder closes, the barrier is pending (P and D are blit, dispatch and fragment) |
| copy B | a new compute encoder opens and emits `barrierAfterQueueStages` for blit and dispatch; fragment stays owed; `copyFromBuffer` |
| barrier 2 | sources inside `S_open`, destinations inside `D_open`: held back as an encoder-level barrier; the fragment part joins the pending barrier |
| `end` | the encoder closes and the held barrier joins the pending one; the trailing encoder emits two barriers; `endCommandBuffer` |

That is three native barriers on three encoders for the two batches.

## Scopes

Outside a pass, `beginScope` runs the core's `beginScope`, closes the open encoder and calls
`pushDebugGroup` on the command buffer with an `NS::String` from `alloc()->init` over the scope's
label, copied to a NUL-terminated scratch string first. A label that is not valid UTF-8 names its
group `rojorhi.scope.unnamed`. The list keeps the string on a stack that names the encoders opened
inside the scope; `endScope` closes the encoder, calls `popDebugGroup` and releases the string.
`scopeTimings` stays the core's, and reports no durations.

## Counts

`nativeRecordCalls` counts every message a list sends after `beginList` to its command buffer, an
encoder or an argument table: `beginCommandBuffer`, `setLabel`, the encoder factories,
`setArgumentTable`, `setAddress`, `setTexture`, the pipeline, state and store-action setters,
debug groups, copies, fills, dispatches, draws, executes, `useResidencySet`, barriers,
`endEncoding` and `endCommandBuffer`; a command deferred to the render encoder's open counts when
it is replayed. Getters and the setters of a pass descriptor count nothing. A render encoder
counts in `encodersOpened` like any other, those of a split and the compute encoder of a counted
draw's kernels included; the pass and draw sites are in [drawing](metal4-drawing.md).
`nativeBarriers` counts each native barrier command, so one batch may count several.
`encodersOpened` counts every native encoder, the trailing one and a clear's render encoders
included. An allocator reset counts one `nativeHostCalls`. `nativeSubmitCalls` counts native queue
calls: the commit of a submission's lists is one, each fence wait and signal the core sees is one,
and the queue's progress signal is one, so a submission of one list signalling one fence counts
three. The core folds a list's counts into the device at submission. Operation counts, inside one
open encoder: `uploadFrameData` makes no native call; a buffer copy is one; an equal-byte fill is
one and an unequal one five (kernel table, two addresses, dispatch, list table; six for the first
kernel an encoder runs, which also sets the pipeline); a barrier between copies is one native call
and one native barrier at its emission.

## Commit timing

`Backends/Metal4/Source/CommitTiming.h` declares a private, test-only seam that is not part of the
public interface. `rojoRHI::metal4::setCommitTiming(queue, callback, user)` makes every later commit
of `queue` carry `MTL4::CommitOptions` with a feedback handler that calls `callback` with a
`CommitTime`: the queue, a serial counted per queue from 1, the GPU start and end in seconds and a
failure flag. It runs on Metal's completion thread, so RojoRHI starts no thread (ADR 0013). A null
callback detaches. The calls on the options are not native calls and are not counted.
`Queue::submit` makes exactly one commit per submission that names lists, and the backend commits
nothing else on a caller's queue except the empty commit that `destroyDevice` drains with, which is
never timed. The stager's internal queue is unreachable from it. The benchmark harness is its user,
as are the `[metal4]` tests.

## Tests

`Tests/Metal4/` holds the `[metal4]` cases of this page: lists and their counts, the encoder,
copies, barrier forms by native counts, kernels and the fill, clears, per-operation counts and
commit timing. `Tests/Conformance/` holds T1 cases that read results back through
`Tests/Support/GpuKit`: copies, initial data, barriers, fills, clears and queue handoff
([testing](../conventions/testing.md)); the cases of passes, draws and dispatches are listed in
[drawing](metal4-drawing.md#tests).
