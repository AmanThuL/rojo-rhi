# The Edges of the Null Backend

**Status**: Accepted

This page continues [the Null backend](null-backend.md) and
[command recording](null-backend-recording.md) with the last four chapters of the reference:
swapchains and presentation (11), upscalers (12), sparse resources (13) and diagnostics (14), which
holds access declarations, scope timings, capture and counters. It ends with the validation
tracker as built so far. The layering is unchanged: every
check and every piece of state lives in `Source/Core`, `Backends/Null/Source` forwards and
contains no `ROJORHI_VU`, and the API reference stays the authority on semantics. Where a decision
below says "Null" it describes this backend only; a native backend decides the same point for
itself.

## Files

| Area | Core (`Source/Core/`) | Null backend (`Backends/Null/Source/`) |
|---|---|---|
| Swapchains | `Swapchains.*` | `NullDevice.*`, `NullQueue.cpp` |
| Upscalers | `Upscalers.*` | `NullDevice.cpp`, `NullCommandList.cpp` |
| Sparse tiles | `Sparse.*`, `Formats.cpp` | `NullDevice.cpp`, `NullQueue.cpp` |
| Access declarations | `Accesses.*` | `NullCommandList.cpp` |
| Scope timings | `Scopes.*`, `ScopeLog.*` | `NullDevice.cpp` |
| Capture | `Captures.*` | `NullDevice.cpp` |
| Counters | `Counters.h`, `DeviceCore.*`, `HandlePool.h` | the sites named below |
| Validation tracker | `Tracker/*` | `NullQueue.cpp` |

## Swapchains and presentation

A Null swapchain ignores the native surface and displays nothing. `SwapchainImpl` embeds a core
`SwapchainCore`: the description, the current extent (`kNullSurfaceExtent`, 1280 by 720, until a
`resize`), the state of each image and the index of the next one. `acquire` hands the images out in
order and `present` takes the held image back. `maxLatency` is stored and never enforced, and
`acquire` never waits or returns a null texture.

Images are fresh texture records per acquire. A Null swapchain holds no standing image textures:
`acquire` allocates a `TextureRecord` marked `swapchainImage` with the usage and format chapter 11
gives it, and `present` releases it through the deferred path. The handle an earlier acquire
returned is therefore stale once the image is presented, as chapter 11 says, and `resize` has
nothing to retire because it requires that no image is held (`VU-rojo-present-0008`). A Presented
state collapses into Available, since a Null present completes inside the call, as a submit signals
its fences inside the call.

`VU-rojo-present-0001` (a non-null surface) is checked on Null. The Debug build rejects a null
`nativeSurface` even though the backend never reads it, so a consumer passes any non-null pointer;
Release accepts null. `VU-rojo-present-0002` (the surface is of the compiled backend's type)
cannot fail on Null. The core holds an optional predicate, `DeviceCore::setSurfaceCheck`, which a
backend with a recognizable surface type registers once at creation; Null registers none and
accepts every pointer, and its death test installs a rejecting predicate. A device keeps the set of
live swapchains for `VU-rojo-present-0006` (one swapchain per surface), under its own mutex.

`VU-rojo-present-0012` (a swapchain image is only a color attachment, a resolve target or a barrier
subject) has one site, `checkNotSwapchainImage`, asked by `createView`, `writeView`, the buffer and
texture copies, `clearTexture`, `tileShape`, `mapTiles` and `upscale`; each caller has a death test.
`VU-rojo-present-0013` is checked in `destroyTexture` and `VU-rojo-sync-0005` in the barrier
check. `VU-rojo-present-0016` is "an image is held" and `VU-rojo-present-0017` "a frame is open,
and it is the frame of the acquire". A destroyed swapchain waits in a per-frame-slot list in
`DeviceImpl` and is freed when the `frameSlot` hook reuses the slot.

## Upscalers

Every upscaler kind the Null device reports can be created. `Auto` resolves to the first kind of
the capabilities list, which is MetalFX on Null, and the `UpscalerRecord` keeps the resolved
kind. `upscale` checks its arguments and records nothing: it calls `checkNotSwapchainImage` on
each of its textures and the format, extent, aliasing and once-per-frame rules
(chapter 12). The once-per-frame rule (`0008`) compares the frame stamp of
the record with an atomic exchange that runs last, so a rejected call does not count, and exists in
Debug builds only. An upscale also records a scope named by `UpscaleParams::label`, or
`rojorhi.upscale.unnamed`.

## Sparse resources

Tile shapes come from the D3D12 standard table, `standardTileExtent` in `Formats.cpp`, beside the
format table. A shape is a backend value, so the table is the Null backend's choice, registered
through `DeviceCore::setTileExtents`; `Sparse.cpp` derives the rest from it. A tile is 64 KiB.
Block-compressed formats take the standard shape of their block size in blocks, `D32FloatS8` tiles
as 64 bits per texel, and a one-dimensional texture tiles as a row. The table holds single-sample
shapes only, because `VU-rojo-texture-0008` forbids Sparse with more samples. `mipTailFirst` and
`mipTailBytes` follow from the extent.

`Queue::mapTiles` keeps each resource's mapping in its record (`TextureRecord::mapped`,
`BufferRecord::mapped`) and the count of mapped tiles in `HeapRecord::mappedTiles`, which
`destroyHeap` reads for `VU-rojo-device-0009`. It takes the pool locks, which ADR 0020 allows
because it is not recording, and it keeps its state in Release as in Debug; only the checks compile
out. A tile mapped again to another heap moves its count, and unmapping an unmapped tile is
accepted. `VU-rojo-sparse-0012` compares the resource and tile key of every element of one call.

Destroying a sparse resource unmaps its tiles. `destroyTexture` and `destroyBuffer` move the
mapping out of the record and drop the heap's counts at the call, so a heap can be destroyed as
soon as the resources that mapped into it are. Chapters 02 and 13 say so.
`VU-rojo-sparse-0011` is retired: every element reports its own rule.

## Access declarations

`CommandList::declareAccess` is inline in the header. Its body branches on `NDEBUG`: in Debug it
calls the private `declareAccessChecked`, in Release it is empty, so a Release call costs nothing
and takes no lock. Consumer and library must agree on `NDEBUG`; a Debug library with a Release
consumer silently skips declarations. The core function checks `VU-rojo-diag-0001` to `0007` only
on a device created with validation, and `0004` is skipped there too, because chapter 14 says the
call returns at once without validation. It keeps the declaration in a list-local pending set
(`CommandListCore::pendingAccesses`) for the tracker. The set is consumed at the draw and dispatch
entries, which share one `consumeAccesses` site (an empty set costs nothing in Release), and
checked at `end` (`0007`).

## Scope timings

`Device::scopeTimings` reports the scopes of the latest retired frame: label, frame, queue type and
depth, in queue type order and submission order within a queue, each with a duration of zero.
Scopes come from `beginScope`, render passes (default label `rojorhi.pass.unnamed`) and upscales.

- **Recording.** `recordScope` copies the label into the list's frame-arena page, through the
  cursor `uploadFrameData` shares, and keeps a view of it in the list. A page is claimed only
  when the current one is full or missing, which is the lock of ADR 0020, so a list that records
  any labelled scope claims at least one page per frame. An empty label takes a static default, so
  no copy is made.
- **Submit.** `Queue::submit` adds the list's scopes to the slot's record in the `ScopeLog`, under
  the log's mutex. That is a queue-class call, not recording, so the rule of ADR 0020 holds.
- **Retire.** The arena rewinds a slot at `beginFrame`, and the lists of the new frame then
  overwrite those pages, so views into them do not live as long as the report. Before the rewind,
  `ScopeLog::retire` copies each label into a string the log owns and builds the report from them.
  The report stays valid until the next `beginFrame`, which clears it.

## Capture

Whether capture exists is an optional hook, `DeviceCore::setCaptureAvailable`, which Null leaves
unset. `beginCapture` checks `VU-rojo-diag-0009` (a path), returns `ErrorCategory::Unsupported`
without a hook, and otherwise claims the in-progress state with one exchange, so two racing calls
cannot both pass `VU-rojo-diag-0008`; a hook that refuses releases the state. `endCapture` clears
it and checks `VU-rojo-diag-0010`. Null therefore always reports that it has no capture tool.

## Counters

`Counters.h` is the public struct and `Device::counters` a snapshot of relaxed atomic loads that
takes no lock. The fields are always compiled in. A recording entry adds to plain fields of its
command list (`RecordCounts`), which fold into the device's atomics at `Queue::submit`, so a list
that is recorded and held shows nothing yet. Creation and queue counts increment where they
happen. Each field counts what the Metal 4 mapping row of the entry does: one native call for each
entry whose row names a call.

| Field | What increments it |
|---|---|
| `nativeCreateCalls` | A successful create entry (a view and a sampler included, since their Metal 4 rows are `newTextureView` and `newSamplerState`), a swapchain, a frame-arena page, and each native release when a deferred destruction runs, a destroyed view or sampler included (not a swapchain image, and not the range a `release` call returns) |
| `nativeRecordCalls` | Each recording entry whose row names a native call: state sets, draws, dispatches, the indirect forms, copies, clears, pass begin and end, scope begin and end, `upscale`, and one per `barrier` call with records; not `setPrimitiveTopology`, `declareAccess`, `uploadFrameData` or `end` |
| `nativeSubmitCalls` | `Queue::submit`, `present` and `mapTiles`, and each fence wait or signal a submit performs |
| `nativeHostCalls` | `completedValue`, and `scopeTimings` when the report is not empty |
| `cpuAllocations` | Explicit sites only: the host block of an `Upload` or `Readback` buffer, an arena page, a new command list, a swapchain; container growth is not counted |
| `gpuAllocations` | A buffer, texture, heap or arena page, and a swapchain image at each acquire |
| `frameDataUploads`, `frameArenaBytes` | `uploadFrameData` calls and the bytes copied, without padding; scope labels count in neither |
| `frameArenaGrowths` | A new arena page, whether an upload or a label caused it |
| `descriptorWrites` | `createView`, `writeView`, `createSampler`, `writeSampler`; the two writes into a reserved range are no native create |
| `globalBarriers`, `bufferBarriers`, `textureBarriers` | The records of each kind passed to `barrier` |
| `nativeBarriers` | One per `barrier` call with at least one record, since a call is one native command |
| `pipelinesCreated`, `pipelineCacheMisses` | Each pipeline; Null has no cache, so `pipelineCacheHits` stays zero |
| `pipelineCompileNanoseconds` | The `steady_clock` time of the core's pipeline preparation; not exact |
| `encodersOpened` | `beginRenderPass`, since Null is not Metal 4 |
| `deferredDestructionsRetired` | Each deferred destruction a retire hook runs, and each destroyed view or sampler a retire frees |

Three counts live in the core and need no Null code. `HandlePool::countInto` ties a pool's
allocations to up to two device counters when the `DeviceCore` is constructed, which counts
creation, `nativeCreateCalls`, `gpuAllocations` and `pipelinesCreated` however a backend allocates;
`allocate(record, counted = false)` lets a swapchain image count in `gpuAllocations` only.
`FrameArena::growths()` is an atomic read. Null adds only what the core cannot see: the four
`cpuAllocations` sites above, which are host allocations the backend makes itself, and the
release of a swapchain, whose retire loop is Null's. A count taken in the core is taken once for
every backend; a count in a backend is that backend's own site. `Device::wait` and the drain at
`destroyDevice` count nothing.

## The validation tracker

[ADR 0021](../decisions/0021-validation-tracker-replays-at-submission.md) decides its shape; the
[API reference](../spec/14-diagnostics.md) is the authority for the rules. What exists is the
layout part: the log, replay at submission, the device mutex and one layout per texture
subresource. The tracker lives in `Source/Core/Tracker/`. Its objects are compiled into every
build, but its paths run only in Debug, on a device created with `DeviceDesc::validation`; the
Release hooks are empty. The Null backend adds two calls to `Queue::submit`, `lockTracker` and
`replayLists`, and no check.

**The log.** Each `CommandListCore` owns a `TrackerLog`, cleared by `beginList` with its capacity
kept. It holds a vector of events, each a `UseEvent` or one of the public barrier records unchanged
(global, buffer, texture). A `UseEvent` names a buffer byte range or a texture and its
`SubresourceRange`, the stage and access of the use, the set of layouts that permit it and the
`UseRule` that a texture use reports; replay learns the Copy queue from the list's queue type.
`UseRule` is an enumeration, not a string, because `ROJORHI_VU` needs an identifier literal at each
site; replay switches on it. A buffer use carries no layouts and `UseRule::None`. The log also keeps
the positions of the open pass's attachment events, which the draws complete.

**The hooks.** `logUse`, `logBarriers`, `logPass`, `logDraw` and `endPassLog` are inline, with
bodies empty under `NDEBUG` and an out-of-line `...Checked` function that Debug calls, the pattern
of `consumeAccesses`. They append nothing on a device without validation. Entries log after their
own checks, where the resolved addresses and layout byte ranges exist:

- copies, `fillBuffer` and `clearTexture` log their endpoints at the stage `Copy` with
  `CopyRead` or `CopyWrite`;
- `drawIndexed` and `drawIndexedIndirect` log the index bytes read at `IndexInput`, and the four
  indirect commands log their arguments, and their count when it is not null, at `IndirectArgs`;
- `consumeAccesses` turns each declared access into a use at its declared stage and access, so
  `declareAccess` feeds the next draw or dispatch;
- `upscale` logs each input in `ShaderRead` and the output in `ShaderWrite`, all as mip 0 of
  layer 0 (chapter 12);
- `beginRenderPass` logs one use per attachment: a colour attachment in `ColorAttachment` for
  writing, plus reading when `load` is `Load`; a resolve target at `Resolve`; a depth attachment
  for reading, plus writing when `load` or `store` discards or clears it, with the stencil
  operations counted only for a format that has stencil. `logDraw` runs after each draw and ORs the
  pipeline's effect into those events: `ColorAttachmentRead` for a blending target and
  `DepthStencilWrite` for a pipeline that writes depth or has a stencil test with an operation
  other than `Keep`. The pipeline record keeps `blendMask`, `depthWrite` and `stencilWrite` for
  this, so `logDraw` takes no lock;
- `barrier` logs every record of the batch, in the order globals, buffers, textures.

`Queue::present` is not a list entry and logs no event. Texture creation, swapchain acquire,
texture destruction and the release of a presented or destroyed swapchain's image call
`trackTexture` or `forgetTexture`, which start and stop the tracker following a texture.

**Replay and the mutex.** `Queue::submit` runs `submitLists`, which checks list state and folds
counters outside the lock, then `lockTracker`, `checkSubmit` (the wait check), `replayLists` and
`recordSignals`. The lock is held from the wait check to the publication of the signal values, and
is empty unless the build is Debug and the device validates. `replayLists` visits the lists in span
order and the events of each list in log order, through one `std::visit` whose `static_assert`
covers every event kind. Texture creation, destruction, acquire and release take the same mutex
for their single call, and `presentImage` takes it to check the image's layout, then again to
release the image, since one hold would deadlock. A texture or barrier destroyed between recording
and submission is skipped at replay: the lifetime rules, not these, judge it. `beginFrame`,
`endFrame`, `Device::wait`, `completedValue`, `mapTiles` and `destroy` do not take the lock yet;
they read no tracker state until the queues part.

**The layout state.** `TextureState` holds the label, the mip and layer counts and one `Layout`
per subresource, indexed by mip and then layer, keyed in a map by the texture handle's bits. A
texture starts in `Undefined`, or in `ShaderRead` when created with initial data (a non-empty
`initial` span); an acquired swapchain image starts in `Undefined` and is flagged until its first
barrier. A texture barrier sets every subresource of its range to its `newLayout`; no other event
changes a layout yet. State persists across submissions, frames and queues, since there is one
layout per subresource, not per queue.

**Rule precedence.** For each subresource of a use, replay reports the first rule that fails:
`VU-rojo-sync-0006` (a use on a Copy list while the layout is not `General`), then the entry's own
rule (`VU-rojo-cmd-0007` for a colour or resolve attachment, `0010`, `0054`, `0067`, `0069`,
`0070`, or `VU-rojo-upscale-0016`), then `VU-rojo-sync-0004`. A texture barrier reports
`VU-rojo-present-0014` for the first barrier on an acquired image before `VU-rojo-sync-0013`.
`VU-rojo-present-0018` is judged at `presentImage`, before the image is released. Each rule has
one `ROJORHI_VU` site per arm and a death test that breaks only that rule, and a test pins each
precedence.

**Hazards.** Each texture subresource, and each byte segment of a buffer, keeps per queue type its
last write (or layout transition) and the reads since it, each with the destinations of the barriers
replayed over it since and whether their sources cover it (`Tracker/Hazards.*`). A source covers a
use by its stages and the use's write accesses, a destination by the use's stages and, after a
write, all its accesses; after a layout transition a barrier orders the next use only when its
source shares a stage with the transition's destination. A buffer's bytes are ordered disjoint
segments that a use or buffer barrier splits at its ends and that merge with their neighbours when
alike and older than the current command (`Tracker/ByteSegments.*`); the tracker follows a buffer
from `createBuffer` to its destruction, with its heap range when placed, and never follows a
frame-arena page. Each event of the log carries its command's ordinal, the list's count of native
recording calls, so replay gives each command one position on its queue: uses of one command, the
attachment uses of one pass and the records of one barrier call share a position and are never
compared or chained with each other. A use reports `VU-rojo-sync-0021` when no barrier lies between
it and an earlier use it conflicts with, and `VU-rojo-sync-0024` when barriers lie between and none
orders the pair; a layout transition reports `VU-rojo-sync-0024` when its own source misses a use
before it. A draw or dispatch with a zero count makes no use, and a pipeline whose stencil write
mask is zero does not write stencil. `Tracker::lastWriteOrder` answers, for a buffer range, how its
last write is ordered before a given use.

**Indirect reads.** The four indirect commands tag their argument and count reads with their
entry and arm. Before the hazards of such a read, replay asks `lastWriteOrder` about its bytes and
reports the entry's identifier (`VU-rojo-indirect-0005`, `0011`, `0017` or `0020`, one site per arm)
when no barrier since the last write on the queue has a destination stage and access that include
`IndirectArgs`; a barrier with that destination whose source misses the write is left to
`VU-rojo-sync-0024`. Bytes only the CPU wrote, initial data or frame data, have no write.

**Aliasing.** Each heap holding a followed placed resource keeps ordered disjoint byte ranges
(`Tracker/OffsetMap.h`) that record per queue type the placed resource that last used them, that
use with the global barriers since, and a stamp ordering heap uses across queues
(`Tracker/Aliasing.*`); `destroyHeap` drops it. A buffer's use maps its bytes into the heap; a
texture's use covers its whole heap range, its placement `heapSize`. A buffer's bytes last used on
the queue by another resource report `VU-rojo-sync-0009` unless a global barrier since covers that
use at its source and this one at its destination, or the buffer's own stamps show it used them
since, on another queue. Every other placed texture over the bytes, and one placed later, gains on
each subresource and that queue a mark per heap byte range: the latest foreign use of those bytes,
which a newer one replaces, so marks stay as few as the ranges. Its next event there must be a
texture barrier from `Undefined` whose source covers every mark newer than the texture's own last
use, or `VU-rojo-sync-0017` reports; a covering discard clears its marks on every queue. A passing
check resets the resource's own hazard state on the queue, since the aliasing chain orders its old
uses and their contents are undefined. Precedence: `sync-0017` before `sync-0006` and the layout,
entry and coverage rules; `sync-0009` before the indirect rules. Limit: a discard is not a heap
use, so although on D3D12 it writes compression metadata, a later aliasing resource is checked
only against the last use. Order between queues is `queue-0005`'s.

**References of unsubmitted lists.** A list counts each object it references once (root, address or
handle) in the record's `ListReferences`; submission or recycling removes them. `destroy` reports
`VU-rojo-abi-0003`, `buffer-0019`, then `device-0007`; replay, `present-0019` for a presented image.

**What remains.** The queues part adds the clocks, the layout per queue and the
handoff, `VU-rojo-queue-0005`, `sync-0018`, `device-0006`, `VU-rojo-sparse-0013` and the lock in
the calls named above.

## What remains
- A native backend registers its own surface predicate, capture hook and tile table, and measures
  scope durations (the Null report has none). It reads native objects while recording without
  a lock ([ADR 0020](../decisions/0020-recording-locks-scope-labels.md)), counts each native call
  at the call and may count cache hits. It must release swapchain images, upscalers and tile
  mappings at retirement and at `destroyDevice`.
