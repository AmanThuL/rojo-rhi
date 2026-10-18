# The Null Backend and the Shared Core

**Status**: Accepted

This page describes the v2 implementation as it stands: a backend-neutral core, the Null backend
that implements the public entries on top of it, and the Debug tier that checks valid usage. The
Null backend of [ADR 0008](../decisions/0008-backends-and-vulkan-oracle.md) renders nothing and
needs no GPU; it exists so the contract of
[ADR 0009](../decisions/0009-conformance-and-valid-usage.md) can be tested on any machine. The API
reference under `docs/spec/` stays the authority on every entry's semantics; this page records how
the code realises it.

## What exists

| Area | Where | Content |
|---|---|---|
| Public header | `Include/rojoRHI/RHI.h` | the whole v2 interface in one header, in sections by API-reference chapter: the version macros, then chapters 01 to 15, with `Format` and `CompareOp` before chapter 04, `Capabilities` after chapter 15 and the object handles `CommandList`, `Queue` and `Device` last, because each names a type of a later chapter |
| Private base | `Source/Base/` | `ROJORHI_VU`, `ROJORHI_INTERNAL_ASSERT`, the message sink |
| Shared core | `Source/Core/` | `HandlePool`, `DeviceCore`, `Records.h`, `SlotAllocator`, the format rules, label checks and the checks of buffers, heaps, textures, samplers, ranges, fences, command lists, pipelines, passes, draws, copies and barriers |
| Null backend | `Backends/Null/Source/` | `NullDevice`, `NullQueue`, `NullAddr`, `NullCommandList`, `NullShaderLibrary`: the public entries, translating to the core |
| Tests | `Tests/Contract/`, `Tests/Core/`, `Tests/Null/`, `Tests/Support/` | see [testing](../conventions/testing.md) |

The implemented entries are the device (creation, destruction, frames, capabilities, format
capabilities, `destroy` of every handle type), the process-wide message callback and the resources
of chapters 03, 04 and 10: buffers and addresses, heaps and placement, textures and views,
samplers, reserved slot ranges, queues and fences, plus the recording chapters (shader libraries,
pipelines, command lists, passes, draws, copies and barriers), which
[command recording](null-backend-recording.md) describes, and the swapchains, upscalers, sparse
resources, access declarations, scope timings, capture and counters of chapters 11 to 14, which
[the edges](null-backend-edges.md) describe. Every chapter is implemented; the validation tracker
is complete, and [its page](validation-tracker.md) describes it.

## Two layers

`Source/` holds the backend-neutral core: everything that decides whether a call is legal or what a
frame boundary means. `Backends/Null/Source/` defines the public member functions and does nothing
but translate them into core calls and a Null-specific answer. A backend has no virtual interface
([ADR 0012](../decisions/0012-language-and-dispatch.md)): one backend is linked per build, selected
by the `rojorhi_backend` xmake option, and the public classes wrap an opaque pointer to the
backend's `DeviceImpl`.

`DeviceImpl` embeds one `DeviceCore`. The core owns the handle pools, the frame bookkeeping, the
label rules, deferred destruction and every Debug-tier check on the device. A check never lives in
a backend, so a later backend reuses all of them unchanged and adds only the translation; a
backend must call the core checks from every entry it defines, or the rule goes unchecked there. The
core reaches a device through `coreOf(Device)`, which each backend defines; the contract tests use
the same seam.

`RojoRHI` is a static library of `Source/**/*.cpp` and the selected backend's sources. The public
headers include nothing from either directory.

## The Debug tier

`ROJORHI_VU(cond, id, format, args...)` in `Source/Base/Assert.h` checks one valid-usage rule when
`NDEBUG` is not defined. On failure it sends `<id>: <text>` to the message callback at error
severity and calls `std::abort()`. In a Release build it expands to nothing and evaluates neither
the condition nor the arguments. `ROJORHI_INTERNAL_ASSERT(cond, msg)` guards invariants of the
library's own code that no caller can break; it reports `internal error: <msg> (<file>:<line>)` and
aborts, and is also absent from Release. A creation entry that can fail without misuse returns
`Result<T>` in every build.

The message sink is one process-wide callback behind a mutex. A message emitted while the callback
runs on the same thread goes to the default sink, which writes `[RHI] [<severity>] <text>` to
standard error, so a callback that triggers a check does not re-enter itself.
`setMessageCallback` called from inside the callback is itself a violation (`VU-rojo-core-0008`).

## Handles

A resource handle is a 64-bit value: a 32-bit slot index in bits 0 to 31, a 24-bit generation in
bits 32 to 55 and an 8-bit type tag in bits 56 to 63. Slot index 0 and tag 0 are never issued, so
the zero value is the null handle. Tags are fixed per type: Buffer 1, Texture 2, Pipeline 3,
ShaderLibrary 4, Heap 5, Fence 6, Upscaler 7.

Each device owns one `HandlePool` per resource type, each with its own mutex. A lookup reports the
first failing rule: `VU-rojo-core-0001` for a null handle where a live one is required,
`VU-rojo-core-0002` for a stale generation (destroyed or never issued), `VU-rojo-core-0003` for
another type's tag. Releasing a slot advances its generation at once, so the old handle is
rejected immediately. The slot returns to the free list only after the frame that released it
retires, and a record handed to the retire callback is cleared afterwards. A slot whose generation
would pass 2^24 - 1 is retired for good and never reused; its record still reaches the callback.
A `Swapchain` is a pointer wrapper, not a pooled handle: the backend's `SwapchainImpl` embeds a core
`SwapchainCore`, `Device::destroy(Swapchain)` releases the image it holds and frees it when its
frame slot is reused, and a default-constructed one is rejected (`VU-rojo-core-0004`). `ViewIndex`
and `SamplerIndex` are not pooled handles; they release slots of the heaps described below.

The seven small handle structs and the four geometry structs (`Extent2D`, `Extent3D`, `Offset3D`,
`Rect2D`) sit in the chapter 01 section of `RHI.h`; they have no behaviour beyond their fields.

## Records and the backend payload

`HandlePool<Handle, Record>` stores one record per live object, and `Records.h` names the record of
each type through `RecordOf<Handle>`. A record holds the label, the descriptor fields the core
checks against and the counters its rules need: a `BufferRecord` its size, memory class, usage and
heap; a `TextureRecord` its description and the number of view slots that hold a view of it
(`VU-rojo-texture-0028`); a `HeapRecord` its size, memory class and the number of resources placed
in it (`VU-rojo-device-0008`); a `FenceRecord` its initial, submitted and completed values.
A `ShaderLibraryRecord` holds its entry points and a `PipelineRecord` its kind, stage set,
topology class, attachment formats, sample count and depth-bias flag.
An `UpscalerRecord` holds its label, its description with the kind resolved (never `Auto`) and
the frame it last upscaled in.

Tile shapes are a backend value: `Formats.cpp` holds the D3D12 standard tile extents, the Null
backend registers them with `DeviceCore::setTileExtents`, and `Sparse.cpp` derives the rest of a
`TileShape` (a fixed 64 KiB tile, the mip tail) from them. `Queue::mapTiles` takes the pool locks,
which ADR 0020 allows because it is not recording, and keeps its mapping state in Release as in
Debug.

Every record ends in `void* native`, the backend's payload, which the core never reads. The
backend fills it between `prepareBuffer` (which checks and returns a record with no payload) and
`addBuffer` (which stores it), and likewise for textures.

A backend frees its payload through `RetireHooks`: one function pointer per handle type plus a
`context`, set once with `DeviceCore::setRetireHooks`, and a `frameSlot` function the core calls
when `beginFrame` reuses a slot, after the pools retired (the Null backend returns that slot's
command lists to their pool there). The table is on the cold path. A null entry
means the backend holds nothing for that type. The core calls a hook with the record when the
record's frame retires and, for every live and pending record, once in `beginDestroy`. A hook runs
under its pool's lock and must not call a pool. The `DeviceCore` destructor calls no hook, because
the backend's state is gone by then; it asserts that every pool is drained. The Null backend
registers one hook, which frees the host memory of a buffer.

## Frames and deferred destruction

Frames are numbered from one; frame N uses slot N modulo `framesInFlight`, which is 2 or 3
(`VU-rojo-device-0001`). `DeviceCore` keeps the frame number and an open flag in one atomic word,
so the free-threaded `destroy` reads a consistent pair while the frame class writes it. An object
destroyed while a frame is open is released with that frame's slot; one destroyed with no frame
open is released with the next frame opened. `beginFrame` retires the slot it reuses: every pool
releases the records destroyed at frames up to N minus `framesInFlight`, passing each to the
retire hook its backend registered.
Frame misuse is `VU-rojo-device-0002` to `0004`. `destroyDevice` with validation selected warns
once per live object, by label, then passes every live and pending record through the same hooks
exactly once. The core also keeps the `Capabilities` and the format table, which the backend fills
at creation, so every capability and format query is one core path. Chapter 02 requires that
drain, and the retire hooks above are how a backend takes part in it.

Labels are checked as UTF-8 without NUL (`VU-rojo-core-0006`, `0007`) and an empty device label
becomes `rojorhi.device.unnamed`; any other label is kept verbatim.

## Null semantics

The Null device is deterministic and does no work.

- `capabilities` reports the floor name `"Null"` and the adapter name `"Null"`, every optional
  feature as supported (upload in device memory, placement sparse, mesh task indirect,
  conservative rasterisation) and every upscaler kind except `Auto`.
- `Limits` takes the Floor column of the limits table in
  [formats and limits](../spec/15-formats-and-limits-part2.md) wherever that column gives a number.
  `maxBufferSize` has no number there, so the Null device reports 2^32 bytes, a value above the
  reach of an `Addr` that is not verified against any hardware. `maxViews` and `maxSamplers` in
  `Capabilities` repeat the limits.
- `formatCapabilities` reports, for each format, every capability the reference allows its class
  (`formatCeiling`): the class rules decide, for example, that an sRGB format has no storage use,
  a block-compressed format is sampled only and a depth format is a depth attachment. A real
  backend reports the intersection of that ceiling with its hardware, never more.
  `Format::Undefined` and a value that is not an enumerator are violations
  (`VU-rojo-format-0005`, `0001`).
- `DeviceDesc::validation` is `ErrorCategory::Unsupported` in a Release build. In Debug it creates a
  device that warns of leaks at destruction and runs the validation tracker, which is complete
  ([the tracker](validation-tracker.md)).
- A pipeline cache blob in `DeviceDesc` is read as described in
  [command recording](null-backend-recording.md): a blob this backend wrote is carried forward,
  any other is discarded with a warning.
- `Device::queue` returns one queue per `QueueType`, owned by the device.

The Null backend reports the shape a Metal 4 or D3D12 device may report, but it reports a superset
of features that no real device reports together: every optional feature supported and every
format capability the class ceiling allows. It claims no capability the reference does not define.

## Buffers and addresses

`prepareBuffer` checks the descriptor, the initial data and any placement; the Null backend then
adds host storage and `addBuffer` issues the handle. Only `Upload` and `Readback` buffers get host
memory (zeroed, then the initial data copied in), which is what `Device::cpuPointer` returns; a
`Gpu` buffer keeps its record and records only the length of its initial data, and `cpuPointer` on
one is `VU-rojo-buffer-0014`. The hook frees the memory when the record retires.

The Null `Addr` is not a virtual address. `Device::address` returns `lo` as the buffer's handle
slot index and `hi` as the byte offset, the way D3D12 pairs a resource with an offset, so a caller
cannot come to depend on arithmetic beyond `operator+`. The backend defines `operator+`
(`NullAddr.cpp`) and the core owns the bound: `checkAddrOffset` rejects an offset, and the sum,
above `maxAddrOffset`, which is 2^32 - 1 here (`VU-rojo-buffer-0011`). The offset is checked alone
first so the sum cannot wrap.

## Heaps and placement

A Null heap owns no memory beyond its record. `prepareHeap` checks the description. A resource
created with a `Placement` goes through `checkPlacement`, the one site of `VU-rojo-heap-0003` to
`0006` for buffers and textures alike: inside the heap, offset aligned, memory class equal to the
heap's, and not Sparse. `placementRequirements` is computed by the backend, because size and
alignment are backend facts: the Null device rounds the byte size of a buffer, or of a texture's
mips, layers and samples, up to a fixed alignment and uses that alignment. A placed resource
counts itself in its heap's `placedResources`, and `destroyHeap` refuses a heap that still has any
(`VU-rojo-device-0008`).

## Textures and views

A texture is a record and nothing else: it stores no texels, so initial data is checked
(`checkTextureDesc`, the stride and size rules, `VU-rojo-texture-0040` for `D32FloatS8`) and
dropped. A view is only a slot. `createView` checks the descriptor against the texture's record,
takes a slot of the view heap and counts the view in the texture's `views`; `destroyTexture`
refuses a texture whose count is not zero. The format arms of `VU-rojo-format-0003` and `0004` for
textures and views are checked in the same functions, against the core's format table.

## View and sampler slots

`DeviceCore::views()` and `samplers()` are two `SlotAllocator`s, one per global heap, bounded by
`maxViews` and `maxSamplers`. A slot is Free, Assigned or Released. A release records the frame
that is open (or the next one), and `beginFrame` calls `retire`, which returns a slot to the free
list only when its frame has retired, so a slot is never handed out again while unretired work may
read it. Each slot keeps an `owner` word; for views it is the texture the slot describes, so
releasing a view slot or range lowers that texture's count.

`reserveViews` and `reserveSamplers` take a contiguous run of free slots, retired or never used,
as one range that belongs to the caller until `releaseRange`. `writeView` and `writeSampler` write
one slot of a reserved range. A full heap, or no run long enough, is an error result, not a
violation. A range is checked by value: `holdsRange` finds that a range with this base and count is
reserved and unreleased, but a stale `IndexRange` equal to a later reservation of the same slots
cannot be told apart. That is the limit `VU-rojo-texture-0031` states for this Debug-tier rule;
using a released range beyond it is undefined behaviour.

## Queues and fences

`QueueImpl` knows its type and its device, so a queue used with another device is rejected
(`VU-rojo-core-0005`). A `FenceRecord` holds the initial, greatest
submitted and completed values. `checkSubmit` compares each wait value with the greatest value
submitted (`VU-rojo-queue-0007`) and each signal value with the initial and submitted values
(`VU-rojo-queue-0003`), and changes nothing. The Null queue executes nothing, so `recordSignals`
with `Immediate` completion raises the submitted and completed values in one locked update per
signal; a concurrent reader never sees a value submitted but not completed. A later backend
passes `Later` and reports each value through `completeFence` as its work finishes. A completed
value is always readable, and `Device::wait` never blocks on Null: `checkWait` returns whether the
value is reached and rejects an unlimited wait on an unreachable value (`VU-rojo-queue-0008`).
`Queue::submit` checks its lists first, as [command recording](null-backend-recording.md) says.

## Coverage

`Tools/check_vu_coverage.py` counts the `ROJORHI_VU` call sites of each identifier in `Source/`
and requires at least as many `[death]` cases as sites, one case per site. It fails on any
`ROJORHI_VU` under `Backends/`, and on an identifier that a check or a death test names but the
reference does not define or has retired ([testing](../conventions/testing.md)).

## Threading

[ADR 0013](../decisions/0013-threading.md) applies: RojoRHI creates no threads. Its clause on
recording is superseded by [ADR 0020](../decisions/0020-recording-locks-scope-labels.md): Release
recording takes a lock only to claim a frame-arena page, for frame data or a scope label, and Debug
recording also takes the pool locks of its checks' lookups. The free-threaded `destroy` locks the
one pool it touches, with one mutex per pool, and reads the frame word atomically under that lock.
The entries that reach two locks
(destroying a placed buffer or texture, which updates its heap after its own pool; `createView`,
`writeView` and `destroyView`, which pair the view slot allocator with the texture pool) release
the first before taking the second. One pair nests: `FrameArena::claim` holds the arena's mutex
while the backend creates a new page in the buffer pool, so the order is always the arena's mutex,
then the buffer pool's, and nothing takes them the other way round. Pool hooks and callbacks never
call a pool.
Capability queries read data fixed at creation and touch no pool. Each slot allocator has its own
mutex, and a fence read copies the record under its pool's lock. The frame class (`beginFrame`,
`endFrame`) is externally synchronised by the caller, as chapter 01 requires, and takes no lock of
its own beyond the pools it retires. The message sink has its own mutex, and the inside-callback
flag is thread-local.

## What remains

These arrive with the work that adds them; this page then describes them. What recording still
lacks is on the [command recording](null-backend-recording.md) page.

- Native payloads for views and samplers: their slots carry no backend payload, retire hook, drain
  or label yet. They arrive with a native backend, which must release native views and samplers at
  retirement and at `destroyDevice`.
- A Metal 4 v2 backend and T1 GPU conformance
  ([ADR 0009](../decisions/0009-conformance-and-valid-usage.md)).

`Tools/vu_pending.txt` lists each identifier whose death test is still to come, with the part that
will add it; it is empty today ([testing](../conventions/testing.md)).
