# The Metal 4 Backend: Presentation, Sparse Resources and Capture

**Status**: Accepted

This page describes the parts of the Metal 4 backend that record no commands: swapchains and
presentation, placement sparse resources and programmatic capture. [The Metal 4
backend](metal4-backend.md) describes the rest of the backend, its lock order and its counting;
chapters 11, 13 and 14 of the API reference stay the authority on semantics and mapping, and
[the Null backend's edges](null-backend-edges.md) describe the shared core these entries call.

## Presentation

A swapchain is a `CAMetalLayer` (`desc.nativeSurface`). `createSwapchain` calls
`core::prepareSwapchain` first, so the surface check of `VU-rojo-present-0002`
(`metal4::isMetalLayer`, `isKindOfClass:` through the Objective-C runtime) runs before the layer is
read. It then makes the Core Graphics color space (`kCGColorSpaceSRGB`,
`kCGColorSpaceExtendedLinearSRGB` or `kCGColorSpaceITUR_2100_PQ`; a space Core Graphics cannot
make returns `Unsupported` before the layer or the core's live set changes) and sets, in
`configureLayer`, the device, pixel format, color space, `wantsExtendedDynamicRangeContent` (for
the two extended spaces, through the Objective-C runtime because metal-cpp has no wrapper),
`maximumDrawableCount` to `imageCount`, display sync for `Fifo`, framebuffer-only and
`allowsNextDrawableTimeout`. The extent is the layer's `drawableSize`, rounded to whole pixels.
The layer properties are set on the calling thread: the backend never hops to the main thread,
and the caller serialises its own use of the layer.

**Presentation state.** `DeviceImpl::presentation` (`metal4::Presentation`, `Metal4Presentation.h`)
holds the live swapchains, the swapchains destroyed per frame slot (`retiredSwapchains`), the
queued residency-set changes, the per-set attachment counts, the drawables acquired since the
graphics queue last submitted, and an atomic `pending` flag, all under its `mutex`.

**Layer residency.** The layer vends its own `residencySet()` once it has a device. Attaching it
to the graphics queue (`addResidencySet`) and detaching it (`removeResidencySet`) are calls on a
queue that creation and destruction may not make while a submission runs, so they queue a change
and set `pending`. `flushPresentation` applies the changes in order on the graphics queue, and
`Queue::submit` (after its signals are recorded, before its first wait) and `Queue::present` call
it first; a queue that is not the graphics queue returns at once, and a clean `pending` costs one
acquire load. A count per set keeps a set attached while any swapchain on that layer lives:
`addResidencySet` runs when the count goes 0 to 1 and `removeResidencySet` at 1 to 0, each one
`nativeSubmitCalls`. `releaseSwapchains` applies what is left, uncounted, at `destroyDevice`.

**Acquire.** `Swapchain::acquire` runs `core::checkAcquireImage`, then returns a null texture
without a native call when the swapchain's extent is zero (a layer set to 0 x 0 still vends
drawables of its old size). Otherwise one deadline of one second, `kAcquireDeadline`, bounds two
waits: first for the latency limit (below), then for `nextDrawable`, which runs in a loop, each
call one `nativeHostCalls` in its own pool. A drawable of another extent, queued across a resize,
goes back to the layer unpresented and the loop asks again before the deadline; nil gives null.
`nextDrawable` keeps Core Animation's own one-second timeout, so a null result takes about two
seconds at worst, and it changes no state. On success `core::acquireImage` takes the drawable's
texture, retained, as the image's native payload: committed work does not retain it, so the image's
record holds it until it retires through the frame. The swapchain keeps the drawable itself
(`SwapchainNative::drawable`) until present and files it in the presentation state.

**The drawable wait.** Metal needs the queue to wait for a drawable before work that writes it.
The next call that reaches `flushPresentation` on the graphics queue issues one
`MTL4::CommandQueue::wait(drawable)` for each filed drawable, counted as one `nativeSubmitCalls`
each, under the presentation mutex in the same locked section that applies the residency changes,
so nothing filed meanwhile is lost. Inside `Queue::submit` that section runs under the tracker's
lock. A compute or copy submission issues none; the second graphics submission after an acquire
finds the list empty.

**Present.** `presentSwapchain` runs `core::presentImage` (which releases the image's texture,
deferred, so committed work still holds it), flushes, then `signalDrawable` (one
`nativeSubmitCalls`), `addPresentedHandler` and `present()` (one `nativeHostCalls`), and releases
the swapchain's drawable. A present with no graphics submission since the acquire therefore counts
two `nativeSubmitCalls` (the wait and the signal) and one `nativeHostCalls`.

**Latency.** Each swapchain shares a `metal4::PresentLatency` (an atomic count, a mutex and a
condition variable) with the presented handler. Present adds one; Core Animation runs the handler,
on one of its own threads, after the drawable is displayed or dropped, and the handler subtracts
one and notifies. A windowless layer drops its drawables and still runs the handler. `acquire`
waits until the count is below `maxLatency` or the deadline passes; a count already below the
limit is read without the lock. The handler owns a `shared_ptr` to the state, so it may run after
the swapchain is freed.

**Resize.** After `core::resizeSwapchain`, `Swapchain::resize` waits, when a frame has been
numbered, for the frame event to reach `frameNumber()`, one `nativeHostCalls`: the graphics queue
signals it after every list of the ended frame, so no work still references the old images. Then
it calls `setDrawableSize`. Before the first frame there is nothing to wait for. Both run in a
pool: when the layer frees a queued drawable of the old size, its texture's `dealloc` leaves an
autoreleased device on the caller's thread.

**Destruction.** `Device::destroy(Swapchain)` runs `core::destroySwapchain` and
`metal4::retireSwapchain`, which moves the swapchain to the list of the frame slot the core
reports, under the presentation mutex. A drawable it holds that is still filed leaves the filed
list there: it is never written or presented, so no later submission waits for it, and every wait
already issued belongs to a frame the slot's reuse waits for. `freeSlotSwapchains` frees that list
when the slot is reused (one `deferredRetired` and one `nativeCreateCalls` for each, as on Null): a
drawable the swapchain still holds goes back to the layer unpresented, the layer's set is queued
for detachment and the layer is released.

### Presenting under `OBJC_DEBUG_MISSING_POOLS`

Presenting a drawable makes Apple's own threads print `autoreleased with no pool` lines:
`com.apple.FramePacing.LayerStateSyncQueue` (FramePacing's layer-state tracking) and
`com.Metal4.SubmissionQueue`, where the last release of a `CAMetalDrawable` runs its `dealloc`.
The IOGPU completion threads print them too: `IOGPUMetal4CommandQueue commandBufferComplete:`
returns the command buffer's storage, and the driver drops the last reference to a buffer in its
resource list with no pool; more indirect command buffers mean more such lines. None of them has a
RojoRHI frame on its stack, except that a completion handler's own pool push may be the call that
reports an earlier driver autorelease on the same thread. A RojoRHI frame that only pushes a pool
does not count, since the push autoreleases nothing. A standalone program that keeps pools
throughout prints the same classes. The check is judged by the stack, not by the count (see
[testing](../conventions/testing.md)).

## Sparse resources

`placementSparse` is `MTL::Device::supportsPlacementSparse`, and the page size is the one 64 KiB
page, `MTL::SparsePageSize64` (`sparseTileSizeInBytes` reports 65,536). A format is `sparse` when
the class ceiling allows it and `sparseTileSize` of the 2D type at that page size has a nonzero
width (`metal4::sparseFormat`). A sparse texture sets `setPlacementSparsePageSize`; a sparse
buffer is `newBuffer(size, options, MTL::SparsePageSize64)` at its own length, with a partial last
tile mapping like any other; a `Memory::Gpu` heap sets `setMaxCompatiblePlacementSparsePageSize`
when the device supports placement sparse, since the validation layer refuses a mapping from a
heap without it. Sparse textures and buffers join the residency set like any other resource.
A barrier from `Undefined` on a sparse texture carries `MTL4::VisibilityOptionResourceAlias`, so a newly
mapped tile's first use is ordered after whatever its heap memory held.

`createTexture` stores the shape Metal measured, `measureTiles`, in the record's `tiles`, which
`core::textureTileShape` returns unchanged: the tile extent from `sparseTileSize` for the texture's
type, format and sample count, and `firstMipmapInTail` and `tailSizeInBytes` from the native
texture. Metal reports a first tail level past the mip chain with a nonzero tail size for a
texture whose levels all tile, so the first tail level is clamped to the mip count and the tail
is zero bytes when there is none. A texture type with no tile (width 0) fails creation with
`Unsupported` before the native texture is made.

Measured on one device, an Apple M3 Max (Apple9) on macOS 27, and not part of the contract:

- `sparseTileSize` at 64 KiB does not depend on the type, and no format or type was refused. The
  extents in texels are 128 x 128 x 1 for `BGRA8Unorm`, `RGBA8Unorm`, `RGBA8Unorm_sRGB`,
  `RG16Float`, `R32Float`, `RGB10A2Unorm`, `D32Float` and `D32FloatS8`; 128 x 64 for `RGBA16Float`;
  256 x 256 for `R8Unorm`; 256 x 128 for `R16Float`; and 512 x 256 for `BC1Unorm` and
  `BC1Unorm_sRGB`.
- A 3D texture tiles one slice per tile and its tail spans many tiles: 128 x 128 x 64 with four mips
  has a tail of 2 MiB.
- The 1D types put every level in the mip tail (`firstMipmapInTail` is 0).
- The kit's 2D array of 512 x 512 RGBA8 with ten mips has a tail at level 3 of 64 KiB per layer,
  which equals the shape the core computes. RGBA16F 300 x 200 with two mips does not: the core puts
  the tail at level 2 and Metal at 3, which is why the shape is measured.

**Operations.** Every region, range and heap offset of `MTL4::UpdateSparseTextureMappingOperation`
and `UpdateSparseBufferMappingOperation` counts tiles, not texels or bytes. A texture region is
converted to tiles and issued once for each layer, with the heap offset advancing by the tiles of
a layer; within one operation Metal maps x, then y, then z onto consecutive heap tiles, which is
chapter 13's order. A mip tail is one operation for each layer at level `firstMipmapInTail` with a
region of one row of tail tiles. An unmap names heap offset 0, and a null heap is accepted for
textures and buffers alike, as is unmapping a tile that was never mapped.

**Grouping.** `groupMappings` makes one group for each resource, heap and mode in the order
elements first name them, and issues all texture groups before the buffer groups. Each group is
one native `updateTextureMappings` or `updateBufferMappings` call and one `nativeSubmitCalls`; a
call with no group, such as an empty span, issues and counts nothing. `Queue::mapTiles` runs the
core's checks first, then `resolveMappings` reads the pools and `groupMappings` builds the groups
without touching the native queue, which is why the grouping is a pure function the tests call.
Destroying a mapped resource issues no unmap; the tiles leave when the resource retires. `mapTiles`
does not call `flushPresentation`, so a mapping on the graphics queue issues no pending drawable
waits.

**Ordering.** Metal orders a mapping update against no command buffer on its queue: the
`updateTextureMappings` documentation leaves a barrier against `MTL::StageResourceState` to the
caller, and RojoRHI's barriers never name that stage. Measured on the M3 Max, without that ordering
an update overtook every read of the old tiles that a long copy held back (all rounds, textures and
buffers), and a copy submitted right after an update of a texture wrote before the update applied in
about 0.2% of rounds (not seen in 300 buffer rounds). Chapter 13 orders a call after the queue's
earlier work and before its later work, which a barrier inside the lists cannot give, since they are
recorded before the call is known. So `orderOnQueue` brackets the updates: before them and after
them it signals `QueueImpl::progress` with the next progress value and waits on the queue for that
value. A call counts its groups plus four `nativeSubmitCalls`, and `progressValue` is written by
`mapTiles` as well as by `submit`. The cases "a mapping applies after the work submitted before it
on its queue" and "work submitted after a mapping on its queue writes the mapped tiles" hold the two
sides. The ordering is verified on this one GPU and is re-checked with the work-around under issue
#58.

**Driver fault and its work-around.** On an Apple M3 Max (Apple9, driver bundle
`AGXMetalG15X_M1`, macOS 27, SDK 26.5) a texture mapping update that follows a buffer mapping
update, after a texture update, on one `MTL4::CommandQueue` crashes in the driver, with and
without the validation layer: SIGSEGV reading `0x1848` in
`-[AGXG15XFamilyComputeContext_mtlnext updateTextureMappings:heap:operations:count:]` (+80),
called from `-[AGXG15XFamilyCommandQueue_mtlnext updateTextureMappings:heap:operations:count:]`.

- These sequences crash (tex is a texture update, buf a buffer update, nothing between): tex, buf,
  tex (the heap and the texture may be the same or other ones); buf, tex, buf, tex; tex, buf,
  unmap tex; tex, buf-unmap, tex; tex, tex, buf, tex.
- These pass: buf, tex; tex, buf; tex, buf, buf; buf, buf, tex.
- Any other queue operation between the buffer update and the next texture update avoids the
  fault: a `signalEvent` (even at the event's current value), a signal and a wait, an empty
  commit, or a CPU wait for the queue.
- The state is per queue. Alternations across two queues pass; the positive controls on one queue
  crash in the same run.

Hence two rules in `metal4::mapTiles`: texture groups come before buffer groups within a call, and
the signal and wait that follow a call's updates put a queue operation between a buffer update and
any texture update a later call makes. The regression cases are "a texture mapping that follows a
buffer mapping on the same queue runs" (it crashes when no queue operation lies between the calls)
and "texture and buffer mappings that alternate across queues run". The work-around is verified on
this one GPU.

## Capture

`createDevice` registers `metal4::startCapture` with `DeviceCore::setCaptureAvailable`. The core
checks `VU-rojo-diag-0008` to `0010`, claims the in-progress state and calls the hook; a refusal
releases the state. The hook (`Metal4Capture.cpp`) uses `MTL::CaptureManager::sharedCaptureManager`
inside a pool and starts a capture of the **system default device**
(`MTL::CreateSystemDefaultDevice`), since the manager is process-global, with the `GPUTraceDocument`
destination and the absolute path (`std::filesystem::absolute`, because the document outlives the
working directory). Errors:

| Condition | Result |
|---|---|
| The process cannot write a `.gputrace` document (the manager does not support the destination, typically because `MTL_CAPTURE_ENABLED=1` was not set at launch) | `Unsupported`, naming `MTL_CAPTURE_ENABLED=1` |
| The manager is already capturing | `Unsupported` |
| The path cannot be made absolute, or there is no Metal device | `Unsupported` |
| `startCapture` refuses the descriptor | `DriverFailure`, with the path and Metal's description |

Metal creates missing directories, so only a path below a regular file is refused. A started capture
counts one `nativeHostCalls` in `Device::beginCapture`, and `Device::endCapture` counts one more
when it stops a capture the manager reports. `DeviceDesc::enableCapture` only makes encoders carry
labels.
