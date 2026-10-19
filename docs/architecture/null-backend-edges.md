# The Edges of the Null Backend

**Status**: Accepted

This page continues [the Null backend](null-backend.md) and
[command recording](null-backend-recording.md) with the last four chapters of the reference:
swapchains and presentation (11), upscalers (12), sparse resources (13) and diagnostics (14), which
holds access declarations, scope timings, capture and counters. It ends with a summary of the
validation tracker. The layering is unchanged: every
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
| Scope timings | `Scopes.*`, `ScopeLog.*`, `ScopeRecord.h` | `NullDevice.cpp` |
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
gives it, and `present` releases it through the deferred path. `core::acquireImage` takes the
image's native payload and publishes it before the handle exists; the Null backend passes a null
one. A backend whose native acquire can fail calls `core::checkAcquireImage`, which changes nothing,
before its native acquire and then `acquireImage(swapchain, native)`. The handle an earlier acquire
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
kind. `core::prepareUpscaler` holds the creation checks and the kind resolution and allocates
nothing: a backend creates its native object, stores it in the record's `native` and only then
allocates the handle, so the payload is in place before the handle exists. Null has no native
object and allocates the prepared record at once. `upscale` checks its arguments and records
nothing: it calls `checkNotSwapchainImage` on each of its textures and the format, extent,
aliasing and once-per-frame rules (chapter 12). The once-per-frame rule (`0008`) compares the
frame stamp of the record with an atomic exchange that runs last, so a rejected call does not
count, and exists in Debug builds only. An upscale also records a scope named by
`UpscaleParams::label`, or `rojorhi.upscale.unnamed`.

## Sparse resources

Tile shapes come from the D3D12 standard table, `standardTileExtent` in `Formats.cpp`, beside the
format table. A shape is a backend value, so the table is the Null backend's choice, registered
through `DeviceCore::setTileExtents`; `Sparse.cpp` derives the rest from it. A tile is 64 KiB.
Block-compressed formats take the standard shape of their block size in blocks, `D32FloatS8` tiles
as 64 bits per texel, and a one-dimensional texture tiles as a row. The table holds single-sample
shapes only, because `VU-rojo-texture-0008` forbids Sparse with more samples. `mipTailFirst` and
`mipTailBytes` follow from the extent. A backend that measures a texture's shape at creation stores
it in the record's `tiles` instead, and `textureTileShape` returns that shape unchanged; the Null
backend stores none.

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
  It then calls the `scopeTimes` retire hook once per queue type that has scopes, with that
  queue's records and the report entries just made for them, still before the rewind; a backend
  that times scopes stores the index of each scope's timestamp pair in `ScopeRecord::timestamp`
  while recording and fills the durations there. Null registers no hook, so every duration stays
  zero. The report stays valid until the next `beginFrame`, which clears it.
- **Read.** The core counts nothing for `scopeTimings`; the backend counts its own timestamp
  reads. Null counts one `nativeHostCalls` when the report is not empty, as a native read would.

## Capture

Whether capture exists is an optional hook, `DeviceCore::setCaptureAvailable`, which Null leaves
unset. `beginCapture` checks `VU-rojo-diag-0009` (a path), returns `ErrorCategory::Unsupported`
without a hook, and otherwise claims the in-progress state with one exchange, so two racing calls
cannot both pass `VU-rojo-diag-0008`; a hook that refuses releases the state. `endCapture` clears
it and checks `VU-rojo-diag-0010`. Null therefore always reports that it has no capture tool.

## Counters

`Counters` in `RHI.h` is the public struct and `Device::counters` a snapshot of relaxed atomic loads
that takes no lock. The fields are always compiled in. A recording entry adds to plain fields of its
command list (`RecordCounts`), which fold into the device's atomics at `Queue::submit`, so a list
that is recorded and held shows nothing yet. Creation and queue counts increment where they happen.
The backend counts each native call it makes: the recording counts below are incremented by the
backend's own recording entries (`noteNativeCall` and the barrier and encoder counts), which on Null
sit in `NullCommandList.cpp`, and the validation tracker numbers its commands itself and counts
nothing. The core counts creation, fence and frame-data counts, and one
`nativeSubmitCalls` for each fence wait and signal a submission carries; `core::presentImage` and
`core::mapTiles` count no native call, so each backend counts the calls its present and its
mapping make (Null: one `nativeSubmitCalls` for each). Each field counts what the
Metal 4 mapping row of the entry does: one native call for each entry whose row names a call.

| Field | What increments it |
|---|---|
| `nativeCreateCalls` | A successful create entry (a view and a sampler included, since their Metal 4 rows are `newTextureView` and `newSamplerState`), a swapchain, a frame-arena page, and each native release when a deferred destruction runs, a destroyed view or sampler included (not a swapchain image, and not the range a `release` call returns) |
| `nativeRecordCalls` | Each recording entry whose row names a native call: state sets, draws, dispatches, the indirect forms, copies, clears, pass begin and end, scope begin and end, `upscale`, and one per `barrier` call with records; not `setPrimitiveTopology`, `declareAccess`, `uploadFrameData` or `end` |
| `nativeSubmitCalls` | Native queue calls only: the backend counts each submission it commits and each signal of its own (Null: one per `Queue::submit`; Metal 4: commits, frame signals and a queue progress signal), plus `present` and `mapTiles`; the core adds one for each fence wait or signal a submit carries |
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
| `encodersOpened` | `beginRenderPass` on Null, counted by the backend, since Null is not Metal 4 |
| `deferredDestructionsRetired` | Each deferred destruction a retire hook runs, and each destroyed view or sampler a retire frees |

Three counts live in the core and need no backend code. `HandlePool::countInto` ties a pool's
allocations to up to two device counters when the `DeviceCore` is constructed, which counts
creation, `nativeCreateCalls`, `gpuAllocations` and `pipelinesCreated` however a backend allocates;
`allocate(record, counted = false)` lets a swapchain image count in `gpuAllocations` only.
`FrameArena::growths()` is an atomic read. Null adds only what the core cannot see: the four
`cpuAllocations` sites above, which are host allocations the backend makes itself, the release of a
swapchain, whose retire loop is Null's, and its recording and submission counts. A count taken in
the core is taken once for every backend; a count in a backend is that backend's own site.
`Device::wait` and the drain at `destroyDevice` count nothing.

## The validation tracker

[ADR 0021](../decisions/0021-validation-tracker-replays-at-submission.md) decides its shape. The
tracker lives in `Source/Core/Tracker/`, runs in Debug on a validating device and is complete:
layouts, hazards and barrier coverage, indirect reads, aliasing, the references of unsubmitted
lists, and the order of the queues, frames and sparse mapping. Its pages,
[the validation tracker](validation-tracker.md) and
[across queues](validation-tracker-queues.md), describe it.

## What remains
- A native backend registers its own surface predicate, capture hook and tile table, and measures
  scope durations through the `scopeTimes` hook (the Null report has none). It reads native
  objects while recording without a lock
  ([ADR 0020](../decisions/0020-recording-locks-scope-labels.md)), counts each native call at the
  call and may count cache hits. It must release swapchain images, upscalers and tile
  mappings at retirement and at `destroyDevice`.
