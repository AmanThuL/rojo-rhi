# The Metal 4 Backend

**Status**: Accepted

This page describes the Metal 4 backend of the v2 interface as it stands: the part that creates a
device, reports its capabilities, owns buffers, heaps, textures, views and samplers, and runs
queues, fences and frames, loads shader libraries, creates and caches pipelines, records command
lists (copies, fills, clears, barriers, timed scopes, render passes, dispatches, direct and counted
draws, MetalFX upscales), presents swapchains on a Metal layer, maps placement sparse resources and
captures programmatically ([drawing](metal4-drawing.md), [the edges](metal4-backend-edges.md)). The
API reference under `docs/spec/` stays the authority on every entry's semantics and Metal 4 mapping;
[the Null backend](null-backend.md) describes the shared core this backend translates to. The
backend builds on macOS only and is the default backend there; `--rojorhi_backend=null` selects the
Null backend, and `null` is the default elsewhere.

## What exists

| Area | Where | Content |
|---|---|---|
| Device | `Metal4Device.*`, `Metal4Capabilities.*` | `createDevice`, `destroyDevice`, capabilities, limits, `beginFrame`, `endFrame`, the three queues |
| Formats | `Metal4Formats.*` | the pixel format table and the format capabilities the device reports |
| Buffers and heaps | `Metal4Buffers.*`, `Metal4Memory.*` | buffers, heaps, placement, `address`, `cpuPointer` |
| Addresses | `Metal4Addr.cpp`, `Metal4AddressMap.*` | `operator+`, `core::decodeAddr`, the map from GPU address to buffer |
| Staging | `Metal4Stager.*` | initial data of Private buffers and textures |
| Textures and views | `Metal4Textures.*`, `Metal4TextureDescriptor.*`, `Metal4Views.*` | textures, views, samplers, reserved ranges and the two ID tables |
| Residency | `Metal4Residency.*` | the device's one residency set |
| Queues and fences | `Metal4Queue.cpp`, `Metal4Fences.*` | `Queue::submit`, `createFence`, `completedValue`, `wait` |
| Presentation | `Metal4Swapchain.*`, `Metal4Presentation.*` | `createSwapchain`, `acquire`, `present`, `resize`, the device's presentation state |
| Sparse | `Metal4Sparse.*` | the format probe, measured tile shapes, `Queue::mapTiles` |
| Capture | `Metal4Capture.*` | `beginCapture`, `endCapture` |
| Lists | `Metal4CommandList.cpp`, `Metal4ListState.*`, `Metal4Scopes.cpp` | `acquireCommandList`, `end`, `uploadFrameData`, the frame-arena pages, scopes |
| Timestamps | `Metal4Timestamps.*` | the counter heaps of each frame slot, the timestamp writes of scopes, passes and upscales, the scope-time hook, `scopeTimings`' durations |
| Recording | `Metal4Encoders.*`, `Metal4Copies.cpp`, `Metal4Clears.cpp`, `Metal4Barriers.*` | the open encoder, copies and fills, clears, barrier lowering |
| Kernels | `Metal4Kernels.*`, `Backends/Metal4/Kernels/`, `xmake/kernels.lua` | the embedded library, the fill, clear and counted-draw pipelines |
| Libraries and pipelines | `Metal4ShaderLibrary.*`, `Metal4Pipelines.cpp`, `Metal4PipelineState.*`, `Metal4PipelineCache.*` | `loadShaderLibrary`, `createPipeline`, the depth-stencil cache, the archive cache |
| Drawing | `Metal4RenderPasses.cpp`, `Metal4DrawState.*`, `Metal4GraphicsState.cpp`, `Metal4Draws.cpp`, `Metal4Dispatches.cpp`, `Metal4Indirect.cpp`, `Metal4Icb.*` | passes, the prefix, state, draws, dispatches, counted draws |
| Upscalers | `Metal4Upscaler.*` | `createUpscaler`, `upscale`, the scaler's fence and cleared reactive mask |
| Commit timing | `CommitTiming.h`, `Metal4CommitTiming.cpp` | the test-only seam that reports each commit's GPU time |
| Helpers | `Metal4Common.*` | autorelease pools, string conversion, bounded event waits |

[Command recording](metal4-recording.md) describes the lists, scopes and upscales, and
[drawing](metal4-drawing.md) passes, pipelines and draws. Every entry of the interface is
implemented. The device reports no conservative rasterisation.
`scopeTimings` reports the duration of every scope, render pass and upscale.

## Device objects and teardown

`createDevice` validates the description in the core, takes `MTL::CreateSystemDefaultDevice` and
requires the Metal 4 family, the Apple7 family, read-write texture tier 2, 996 argument-buffer
samplers, a `maxBufferLength` of 2^32 bytes (so `maxAddrOffset` is at least 2^32 − 1, above the
floor of 2^29 − 1), 1,024 x 1,024 x 64 threads per threadgroup, 32,768 bytes of threadgroup memory
and BC texture compression: every floor item of chapters 02 and 15 that Metal reports. The first
missing one is named in an `Unsupported` result. It then creates, in this order: the three
`MTL4::CommandQueue`s (graphics, compute, copy), one `MTL4::Compiler` with its pipeline data set
serializer, the residency set, the stager and its internal queue, the frame event, the two ID tables
and the backend's kernel library with its pipelines, then loads `DeviceDesc::pipelineCache`
([drawing](metal4-drawing.md#the-pipeline-cache)). Each of the three queues also owns a progress
event, described under fences. Any failure releases what exists in reverse and returns the error.
`destroyDevice` drains every queue, the internal upload queue included, by signalling a throwaway
event and waiting for it, then committing an empty command buffer with a feedback handler and
waiting for the handler (Metal making no event or buffer is fatal, since nothing could be released
safely), so the retire hooks that `beginDestroy` runs find idle queues; `VU-rojo-device-0002`
therefore fires after the drain. It then releases the lists' objects, the swapchains still held, the
counter heaps scopes created on first need ([command recording](metal4-recording.md)) and the native
objects in reverse order of creation. The device's own objects and the calls that create, drain and
release them are not counted.

`createDevice` leaves the residency set dirty: the first submission, or the first staged upload,
commits it. Capabilities come from the device and from the Metal 4 column of the limits table of
[formats and limits](../spec/15-formats-and-limits-part2.md); a limit whose column is not a number
reports the floor. The floor name is the highest Apple family the device supports.

`Metal4Formats` starts from the core's class ceiling and removes what the hardware lacks: sparse for
a format the device does not tile ([sparse resources](metal4-backend-edges.md#sparse-resources)) and
sample counts without `supportsTextureSampleCount`. BC formats keep their ceiling, because
`createDevice` requires `supportsBCTextureCompression`. Three narrowings follow the Metal Feature
Set Tables. The shader ABI reads a storage view through `read_write` textures, and read-write
texture tier 2 lists neither `BGRA8Unorm`, `RG16Float` nor `RGB10A2Unorm`, so storage read is off
for all three; they keep storage write, which the write-only accessors reach through `access::write`
textures. `R32Float` filtering and resolve follow `supports32BitFloatFiltering`. Linear filtering of
depth formats needs Apple9. Each line carries its source in a comment.

## Residency

One `MTL::ResidencySet`, labelled `rojorhi.residency`, belongs to the device and is attached to the
three queues and to the internal upload queue. Allocations join it from any thread under its mutex
and mark it dirty, through an atomic flag: every dedicated buffer and texture, every heap, every
frame-arena page and the two ID tables. A placed buffer or texture never joins, because its heap
did. An allocation leaves the set when its retire hook runs.

The set is committed when it is dirty and something needs it: `Queue::submit` commits before it
encodes its waits and signals, and the stager commits before a copy. A commit counts one
`nativeHostCalls`. A submission reads the flag with one atomic load and takes the set's mutex only
when it is dirty, so it pays for residency, lock included, only when an allocation joined or left
since the last commit. The commit opens an autorelease pool, since a submission holds none.

## Native payloads and the native table

Every record ends in `void* native`. `HandlePool` publishes it into a lock-free native table when
`allocate` stores the record, and `HandlePool::native(handle)` reads it with one acquire load,
taking no lock and checking nothing, so an entry that recorded under no lock can reach an object
without touching a pool. The table holds 2^22 slots; a creation that finds it full returns
`OutOfMemory` through `core::tableFull`, and the backend releases the object it made. A buffer's
payload, a `metal4::BufferPayload` (one `cpuAllocations`), caches its GPU address and length.

A pool clears a slot's table entry before the retire hook runs, so a racing misuse that reads the
table gets null and never a dangling pointer, and the hook then releases the object and sets the
record's field to null. A list's cached pipelines do not read the table
([drawing](metal4-drawing.md#state-and-the-prefix)).
A hook opens an autorelease pool, tolerates a null payload and releases in reverse order of
creation. The backend registers hooks for buffers, heaps, textures, fences, views, samplers,
shader libraries, pipelines and upscalers ([command recording](metal4-recording.md#upscalers)), a
`frameSlot` hook that releases the encoders each of a slot's lists ended and returns the lists to
their pool, frees the swapchains destroyed in the slot's frames, readies the slot's timestamps and
releases the fence events the queues have passed, a `scopeTimes` hook that converts the slot's
timestamps and a `waitFrame` hook.

## Addresses and the address map

`Device::address` returns the payload's GPU address plus the offset, split into `lo` and `hi`.
`operator+` checks the offset against `maxAddrOffset` (`maxBufferSize` less one, since no byte of
a buffer lies further from its start, and at least 2^32 − 1 by the floor check; read from a
process-wide value `createDevice` stores) and adds in 64 bits. Decoding an address back to
a buffer slot feeds only Debug checks, so the map exists in Debug builds only: `AddressMap` orders
buffers by base address under its own mutex, `decodeAddr` finds the holder, and a destroyed buffer
still decodes until its slot retires. Placed buffers may alias, so several entries can share a
base; the lookup prefers one that is not destroyed. Release keeps no map, because a lookup that only
feeds a Debug check is compiled out of Release (ADR 0020).

## Initial data

A buffer or texture in Private memory receives its initial data through the `Stager`. The stager
owns a fourth `MTL4::CommandQueue`, `rojorhi.upload`, that no `QueueType` names, with one allocator,
one command buffer and a residency set of its own for the staging buffers, committed, and counted
as one `nativeHostCalls`, each time a staging buffer joins or leaves it. A copy makes a Shared
staging buffer, copies the bytes into it, encodes the copy on a compute encoder, commits on the
upload queue and waits. Creation waits and nothing is committed on a caller-visible queue, so the
data is visible to every later submission.

The wait uses the commit's feedback handler, not a shared event: the handler runs after Metal has
finished the commit and dropped its references, so the stager may release its objects at any time
between copies. A failure, at creation or in execution, is a `DriverFailure` naming
`rojorhi.staging`. A texture's subresources go into one staging buffer at 256-byte aligned offsets
and one encoder copies each into its mip and slice, a cube face counting as a slice. Upload and
Readback memory is Shared and needs no staging. Copies run one at a time under the stager's mutex;
tests read Private memory back through the same path.

## Views, samplers and the ID tables

The device owns two Shared buffers of `MTL::ResourceID`, `rojorhi.viewTable` with `maxViews` entries
and `rojorhi.samplerTable` with `maxSamplers`, both resident through the device set. An index handed
to a shader selects one entry. `createView` and `writeView` make the native view, read its resource
ID, place the payload in the core's slot allocator and only then write the ID, so a slot that was
refused holds no ID. The view is the texture itself, retained, when it has the texture's type, pixel
format and whole range, and otherwise an `MTL::Texture` view labelled `<texture>.view`; stencil
views use `X32_Stencil8`. A sampler is an `MTL::SamplerState` built from a descriptor.

The slot allocator holds the payload (see [view and sampler slots](null-backend.md)). A write
replaces a slot's payload, and the old one waits for the frame of the write; a release hands the
payload to the `view` or `sampler` hook when its frame retires; `drain` hands over everything left
at `destroyDevice`. A refused creation is a `DriverFailure` for `createView` and `createSampler`
and fatal for `writeView` and `writeSampler`, as chapter 04 says.

One reference rule: Metal has no mipmapped 1D textures and aborts when asked for one, so
`VU-rojo-texture-0042` requires `mips` to be 1 for `Tex1D` and `Tex1DArray`. The core checks it,
so every backend rejects the same descriptors.

## Queues, fences and frame pacing

A fence is an `MTL::SharedEvent` created at its initial value and labelled. `Queue::submit` runs the
core's list checks, then under the tracker's lock checks the fences, replays the lists, commits the
residency set when dirty, records the signals as completing later (Debug only: only checks read
them), calls `MTL4::CommandQueue::wait` for each wait, commits the lists' command buffers in one
`commit` and calls `signalEvent` for each signal. `completedValue` reports the event's value to the
core; `Device::wait` follows chapter 10: zero polls, an unlimited wait loops in 10 s slices that
each log a warning with the fence's label, a finite one rounds up to milliseconds.

A fence's native event must outlive every submission that names it. `Device::destroy` defers its
release to a frame slot's retirement, whose wait covers each earlier graphics submission and each
compute or copy one with lists and at most one signal, whose native calls the frame's join orders
(`VU-rojo-device-0006`). A fence-only one joins no frame, and a join through one of several signals
leaves the later ones unordered, so each queue owns a progress event only it signals: such a
submission signals its next value, with no lock, allocation or native read; `mapTiles` signals and
waits on it around its updates ([sparse resources](metal4-backend-edges.md#sparse-resources)).
`Device::destroy(Fence)` snapshots each queue's last progress value and, unless every queue has
already passed it, keeps the event retained; the same call releases the kept events that every queue
has passed, so they stay bounded even when no frame begins. The kept events are released, inside a
pool, when every queue has passed its snapshot; the check runs at each frame-slot reuse, reading a
queue's progress only when a kept event waits for it, and at `destroyDevice`, after the drain.

Frames pace on the graphics queue. `endFrame` signals the frame event, `rojorhi.frame`, with the
frame number (one `nativeSubmitCalls`); it stays, since a frame may end with no graphics signal and
another queue may raise a fence first. `beginFrame`'s `waitFrame` hook (one `nativeHostCalls`) waits
for the frame that used the slot it reuses before retiring it.

## Counting

The core counts what it can see; a backend counts at its native call sites. In the core: creation,
allocation and descriptor counts, `completedValue`, one `nativeSubmitCalls` for each fence wait and
signal a submission carries, frame data and arena growth. In this backend: `nativeSubmitCalls`
counts native queue calls only, so Metal 4 counts its own commits and signals (the stager's upload,
a submission's commit, `endFrame`'s frame-event signal, the progress signal a compute or copy
submission owes (above), a drawable's wait and signal, a residency-set attach or detach, and each
native mapping update) and the Null backend counts its one per `Queue::submit`; a submission that
names a fence therefore counts its waits, its signals, one commit when it has lists and its progress
signal if it owes one, and an empty submission counts none. `nativeHostCalls` counts residency
commits, of the device set and of the stager's own, frame waits, allocator resets, a native
`Device::wait` and the progress reads of a frame-slot reuse and of `Device::destroy(Fence)`, each
`nextDrawable` call, a `present` and the frame wait of a `resize`, and the start and stop of a
capture; the stager counts its staging buffer in `nativeCreateCalls`. Presentation and sparse
mapping add to `nativeSubmitCalls` as [their page](metal4-backend-edges.md) states.
`nativeRecordCalls`, `nativeBarriers` and `encodersOpened` are noted by the backend on the list at
each recording site and folded into the device by the core at submission; the tracker numbers
commands itself and counts nothing. [Command recording](metal4-recording.md) lists the sites.

## Threading and locks

[ADR 0013](../decisions/0013-threading.md) and
[ADR 0020](../decisions/0020-recording-locks-scope-labels.md) apply. Release recording takes no lock
but the frame arena's, to claim a page. Reading a native payload is lock-free, and a list's own
objects need none because only its recording thread touches them.

The backend's mutexes nest in one order, outermost first:

1. the frame arena's, the validation tracker's or the stager's, which never nest in one another;
2. a `HandlePool`'s, such as the buffer pool's;
3. the residency set's, the Debug address map's, `Presentation::mutex`, the depth-stencil cache's,
   the pipeline cache's, the scope log's or the alias tracker's, which are leaves: nothing is taken
   while any of them is held, and none is taken inside another.

The alias tracker's is taken alone, only at placement, unplacement and heap destruction, never
while recording and never inside a pool's mutex.

A thread may skip a level but never takes a lock of an earlier level while it holds one of a later
level. Claiming an arena page holds the arena's mutex while it adds the page to the residency set,
then to the buffer pool, then in Debug to the address map. A retire hook runs under its pool's mutex
and takes the residency set's and, for a buffer, the address map's; it calls neither a pool nor the
arena. A pipeline creation takes the depth-stencil cache's mutex, and `pipelineCacheData` the
pipeline cache's, each alone and only at creation, never while recording; a creation also holds the
pipeline cache's lock shared across its native compile, alone, so it stays a leaf. `createUpscaler`
takes a process-wide mutex of `Metal4Upscaler.cpp` around `newTemporalScaler` with no other lock
held, and inside it the pipeline cache's lock shared (MetalFX fills a process-wide table of Metal's
without a lock, so scalers are created one at a time on every device). Nothing else takes that mutex
and nothing is taken while the cache's lock is held, so the two never nest the other way. The scope
log's mutex is taken alone, by a submission and by `beginFrame`'s retire; the `scopeTimes` hook that
runs under it takes no lock. `Queue::submit` takes the residency set's mutex and, through
`flushPresentation`, the presentation mutex inside the tracker's lock, and a copy takes the
residency set's inside the stager's. The presentation mutex is never held across `nextDrawable` or
the latency wait; `acquire` waits on the latency state's own mutex and condition variable, which are
leaves taken with no other lock held. Creating a buffer or texture takes no two of them at once: the
residency set, then its pool, then the address map, each released before the next.

`Queue::submit` holds the tracker's lock, which only a validating device takes, from the check to
its last native call. Inside it, it takes the fence pool's mutex once for each fence it checks and
once for each signal it records, in the shared core code, as on Null, in Debug the pool mutexes of
the lookups its checks and the replay make, each released before the next, and the residency set's
mutex only when the set is dirty. A clean set costs one atomic load, so residency adds no lock to a
submission once the set is committed.

Two more mutexes are taken alone: each frame slot's list pool's, taken by `acquireCommandList` and
by the frame-slot reuse (acquisition is free-threaded and is not recording under ADR 0020, as on
Null), and the kept fence events', taken by `Device::destroy(Fence)` after the core's destroy has
released the fence pool's and by the frame-slot reuse after it has released the list pool's. The
view and sampler hooks run under their slot allocator's mutex and take none. The timestamp heaps
take no lock: a claim is an atomic add and a heap is published by a compare-exchange.

## Autorelease pools

Every entry that calls an autoreleasing factory opens a pool (`objc_autoreleasePoolPush`) and
retains what it keeps, as does every retire hook and destructor that holds Metal references. The
native commands (copies, the native fill, each kernel dispatch, dispatch and draw, a command buffer
timestamp and the end of a render encoder) autorelease per call only inside the validation layer,
which wraps the device in a debug class. `createDevice` decides once from the class name of the
`MTL::Device` (`metal4::commandsMayAutorelease`) and keeps `DeviceImpl::commandPools`, copied into
each list. Detection fails closed: only a bare driver class name (prefix `AGX`) turns pools off; any
other, null or a wrapper included, opens them. Commands open a `metal4::CommandPool` on it. A label
is +1, but the driver reads its text through an autoreleased container, so each call that passes one
(a list's new label, scope and pass groups, encoder opens) has a pool whatever the flag. `new*`
objects are +1, released once; an `NS::Error*` is reset before each call.

## Tests

The Metal 4 test binary builds `Tests/Contract/` whole, `Tests/Metal4/` (the `[metal4]` cases),
`Tests/Conformance/` (the `[conformance]` cases), `Tests/Support/` (`GpuKit` among them) and
`Tests/Support/Metal4/` (the surface, a readback helper and `runsOnGpu`). A metal4 build needs the
Metal toolchain for the kernels. `Tests/Core/` and `Tests/Null/` build with the Null backend only.
[Drawing](metal4-drawing.md#tests) lists the cases of passes, pipelines and draws. The entries
`contract`, `death`, `abi` and `conformance` run under `MTL_DEBUG_LAYER=1`.
By hand, in Debug, from `build/macosx/arm64/debug/rojorhi-test`:

```
MTL_DEBUG_LAYER=1 ./RojoRHITests "[contract]~[death],[metal4]~[death]"
```

A validation-layer message is a failure, and so is an `autoreleased with no pool` line with a
RojoRHI frame under `OBJC_DEBUG_MISSING_POOLS=YES`; the lines Apple's own threads print once a
drawable is presented do not count
([presentation](metal4-backend-edges.md#presenting-under-objc_debug_missing_pools)). A death test
needs no GPU path of its own: the checks live in the core, so the same cases run on both backends.
[Testing](../conventions/testing.md) owns the tiers.