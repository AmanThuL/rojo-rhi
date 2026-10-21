# Command Recording on the D3D12 Backend

**Status**: Accepted

This page describes how D3D12 command lists record: native lists and allocators, the lazy root
binding, copies, the fill kernel, clears, barrier batches, timed scopes, and the counts each native
call adds. It covers chapters 07 (copies, fills, clears, scopes), 09 (barriers) and 14 (scope
timings) of the API reference on this backend; the device, queues, descriptor heaps and the heap
directory are in [the D3D12 backend](d3d12-backend.md), which this page does not repeat. Draws,
dispatches, render passes and indirect work do not record yet. Everything below is "implemented,
verified on WARP only, not on hardware": the backend is not a capability until a hardware run says
so ([ADR 0009](../decisions/0009-conformance-and-valid-usage.md),
[documentation](../conventions/documentation.md)).

Every entry calls the core's check first (`Source/Core`), then translates, and holds no
`ROJORHI_VU`; [the Null backend's recording](null-backend-recording.md) describes the shared core.
[Metal 4 recording](metal4-recording.md) is the sibling page.

## Lists and allocators

`ListPools` keeps one pool per frame slot and `QueueType`. A pool hands out the lists its slot took
since it was reused and grows by one list when it runs out. A list the pool grows by is one counted
`cpuAllocations` and owns, from then on:

- an `ID3D12CommandAllocator` and an `ID3D12GraphicsCommandList9` of its type (direct, compute or
  copy), the list made closed by `CreateCommandList1`; both are named
  `rojorhi.list.<slot>.<graphics|compute|copy>.<ordinal>`, and each is one `nativeCreateCalls`;
- its `ListState`: the root binding, the barrier scratch, the timestamp heaps and a UTF-16 scratch
  string for labels.

No two lists share an allocator, so recording takes no lock and lists record on any thread.

Acquisition runs the core's `beginList`, then `Reset(allocator)`, and clears the binding and the
timestamp claims. A Graphics list then sets both shader-visible heaps and both the graphics and
compute global root signatures (4 native calls); a Compute list sets the heaps and the compute
global root signature (3); a Copy list records nothing more (1). The native list carries its name
across recordings and `SetName` runs only when the new label differs from the one it carries, so a
list acquired again under the same label sends none.

Reusing a frame slot resets the allocator of every list the slot handed out, one `nativeHostCalls`
each. The frame wait before the reuse proved every list of the slot complete only because the
reference requires every submission to join the frame (`VU-rojo-device-0006`); the backend relies
on that and checks nothing more. `end` runs the core's `endList`, resolves the timestamps (see
scopes) and calls `Close`; a failed `Close`, `Reset` or creation goes through the backend's failure
path, which reports device removal. Lists release with the device after the queues idle.

`Queue::submit` calls `ExecuteCommandLists` once with every list of the submission, from a vector
the queue owns and reuses, between the waits and the signals; a submission with no lists makes no
call. It counts as one `nativeSubmitCalls`. A Compute or Copy submission that joins no frame (no
lists, or more than one signal) also signals the queue's progress fence, which counts as one more.
One call per submission holds on WARP: the cases whose lists open and close with a barrier pass
there, so the backend keeps no per-list fallback.

## Root binding

`setRoot` runs the core's check, resolves the address with `rootAddress` (the directory entry of
`addr.lo`, whose GPU base plus `addr.hi`; 0 when none, which the core rejects in Debug), stores it
in the list's `Binding` and sets both `graphicsRootDirty` and `computeRootDirty`. It makes no
native call. `pushRoot` is the public template over `uploadFrameData` and `setRoot`.

`Binding` also holds `drawIdDirty` (true after acquisition), which root signature the compute side
holds (`Global` or `Kernel`) and the pipeline each side holds. A draw or dispatch will bind the root
address on its side when that side's flag is set; those entries do not record yet. The native list
has one pipeline-state slot for both sides, so setting a pipeline clears the other side's
recorded pipeline.

The fill kernel leaves the compute side in `Kernel` with its pipeline and sets
`computeRootDirty`, and clears the recorded graphics pipeline. The next dispatch therefore sets the
global root signature, its own pipeline and the root again. A fill after a fill finds the kernel
still bound and does not rebind it.

## Copies

`copyBuffer` is one `CopyBufferRegion`. A copy between a buffer and a texture is one
`CopyTextureRegion` per layer, or one for a 3D region, with a placed footprint:

- `Offset` is the layout's offset plus `layer` times the image size, `RowPitch` the layout's;
- `Format` is `copyFormat`, the format `GetCopyableFootprints` reports for subresource 0, stored in
  the texture's payload at creation and never queried while recording;
- `Width` and `Height` are the region rounded up to whole blocks, `Height` being `rowsPerImage`
  rows of blocks when `rowsPerImage` is set; `Depth` is the region's depth for 3D, else 1.

The box names the region's texels in the source, so a buffer-to-texture copy boxes the buffer's
footprint from its origin and puts the texture offset in the destination coordinates. The
subresource index is `mip + layer * mips`; copies exclude stencil formats (`VU-rojo-cmd-0041`).
`copyTexture` is one call per layer, or one for 3D, with a box of whole blocks: a region reaching
the edge of a block-compressed mip may end in a partial block, and the native box counts whole
blocks. Payloads are read with one acquire load and no lock.

## Fills

`fillBuffer` is a compute dispatch of the backend's kernel; D3D12 has no native buffer fill, so the
call costs more native calls than a Metal 4 fill. The kernel is `Kernels/Fill.hlsl` (`cs_6_6`,
entry `main`, 64 threads), taking four root constants at `b0`: `uavIndex`, `pattern`, `firstWord`
and `wordCount`. It stores `pattern` at word `firstWord + i` of the raw buffer at
`ResourceDescriptorHeap[uavIndex]` for every `i < wordCount`, each thread looping with a stride of
`groups * 64`. The host dispatches `groups = min(ceil(wordCount / 64), 65535)` groups; the kernel
recomputes the same value for its stride.

The xmake rule `rojorhi.d3d12_kernels` (`xmake/kernels.lua`) compiles every kernel file with the
pinned DXC (`-T cs_6_6 -E main -HV 2021 -WX`) into a signed DXIL container (`dxil.dll` sits beside
`dxc.exe`) and writes the bytes into `D3D12KernelLibrary.h` in the build directory as
`rojoRHI::d3d12::k<Name>Kernel`; the header is rewritten only when its content changes, and a
missing DXC fails the build naming `xmake setup`. `createDevice` creates the fill's compute
pipeline against the kernel root signature (four constants and the heap flags) and names it
`rojorhi.kernel.fill`; the device fails to open if it cannot.

A buffer's raw UAV reaches its first 512 MiB (see "Addresses" in the backend page), so the range
is split at multiples of 2^29 bytes. The window below 2^29 writes through the buffer's own raw UAV
(`viewIndex + 1`) with `firstWord = offset / 4`. Each later window gets a raw UAV over exactly that
window, `FirstElement = start / 4`, written into a slot claimed from the frame slot's transient
ring (one `descriptorWrites`), and dispatches with `firstWord = 0`. The root signature and
pipeline are set only while the compute side does not already hold the kernel. No frame-arena page
is claimed, so a fill takes no lock.

## Clears

`clearTexture` reaches every subresource of its range through one of three paths, chosen by usage
and queue:

| Texture and list | Native clear |
|---|---|
| `RenderTarget` usage, Graphics list | `ClearRenderTargetView` per mip and layer (per depth slice for 3D) |
| `DepthStencil` usage, Graphics list | `ClearDepthStencilView` per mip and layer, depth, and stencil for a stencil format |
| anything else, and every texture on a Compute list | `ClearUnorderedAccessViewFloat` per mip |

The first two clear through the per-texture views created with the texture
([descriptor heaps](d3d12-backend.md#descriptor-heaps)); a texture with both `RenderTarget` and
`Storage` clears through its render-target views on a Graphics list. The third claims one transient
slot per mip and creates a UAV of that mip over the range's layers (every depth slice for 3D) twice,
into the shader-visible slot and into its CPU-only twin, because the native clear takes both. A
UAV has no cube dimension, so cubes clear as arrays of 2D faces. There are no integer color
formats, so the float clear is the only one.

The subresources are in `CopyDst` when the clear begins (`VU-rojo-cmd-0072`). One `Barrier` before
moves the whole range, both planes of a stencil format, from `COPY_DEST` to the layout, access and
sync of the path; one `Barrier` after moves it back. The `CopyDst` side of each is the list's
`Copy` sync narrowed to `COPY_DEST` (see barriers), which is `SYNC_COPY`, so callers chain
their barriers through `SYNC_COPY`. That is two `nativeBarriers` per clear. A clear that claims
more than 1,024 transient slots in one frame slot, counting fill windows, logs and aborts in every
build.

The debug layer warns `CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE` and
`CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE` when a clear's value differs from the texture's
optimized clear value. `TextureDesc` carries none, so textures are created without one and every
clear differs; both messages are in `kFiltered` in `D3D12Debug.cpp` with that reason. So is the hint
`NON_OPTIMAL_BARRIER_ONLY_EXECUTE_COMMAND_LISTS` (1356) on a list that records only barriers, which
is valid usage: the caller states there the dependencies its later lists rely on.

## Barriers

`CommandList::barrier` records one `Barrier` call per batch with one group per non-empty kind
(globals, buffers, textures), because the layer warns on an empty group; an empty batch records
nothing. Buffer barriers cover the whole buffer: the range is widened, ordering more than asked and
never less. RojoRHI never calls `ResourceBarrier`. The tables of chapter 09 apply, refined as
follows. The syncs below are the lowering before narrowing: each side's sync is then narrowed to
the bits compatible with that side's access, as masked for the queue and the layout (`narrowSync`).
A `SYNC_ALL` sync is kept, a side with `NO_ACCESS` drops only `CLEAR_UNORDERED_ACCESS_VIEW`
(the layer rejects it without an access), and a sync left with no compatible bit becomes
`SYNC_ALL`.

- **Sync.** `Stage::All` is `SYNC_ALL` and `None` is `SYNC_NONE`, except as the source of a buffer
  or texture barrier: GPU-based validation rejects `SYNC_NONE` before a resource any earlier
  command touched, in an earlier `ExecuteCommandLists` too, and the backend does not track that,
  so such a source takes the destination's narrowed sync, or `SYNC_ALL` when the destination names
  no stage either. The barrier then waits for earlier work in the scope it holds back, which costs
  nothing when it opens a submission and otherwise waits only for that scope's earlier work. Out of
  a layout other than `Undefined` a side with no access needs `SYNC_NONE` (the layer rejects any
  other sync with `NO_ACCESS` there), so such a side, source or destination, names the accesses the
  layout admits: those its sync runs, the layout's own scope when it names no stage, or all of
  them with `SYNC_ALL` when its sync runs scopes they do not. The `Copy` stage lowers to the
  syncs a copy, a fill and a clear use on the list's queue: `COPY`, `COMPUTE_SHADING`,
  `RENDER_TARGET`, `DEPTH_STENCIL` and `CLEAR_UNORDERED_ACCESS_VIEW` on Graphics; the first,
  second and last on Compute; `COPY` alone on Copy. `Resolve` is `RENDER_TARGET | RESOLVE`.
- **Access per queue.** A side's access is masked by what the queue performs (a Copy list: copy
  source and destination; a Compute list: also shader, unordered, constant and indirect access). A
  side left with none is `NO_ACCESS`, so a `Stage::All` side is valid on every queue. A side whose
  stage is `None` starts as `NO_ACCESS`; out of a defined layout the sync rule above gives a texture
  side the accesses that layout admits. `Resolve` is `RENDER_TARGET` access. A global barrier no one
  layout admits drops attachment accesses, keeping them in a global per layout that both sides name.
- **`CopyWrite`** is `COPY_DEST`, and also `UNORDERED_ACCESS` on global barriers and on buffers
  with a raw UAV (`ShaderWrite` usage), because the fill kernel writes through that view.
- **Texture reads.** `ShaderRead` is `SHADER_RESOURCE`, or `UNORDERED_ACCESS` when the texture
  side is in `ShaderWrite`, which reads through its Storage view. `CONSTANT_BUFFER` is added on
  global and buffer barriers only.
- **Layouts.** `General` is `COMMON`; `ShaderRead` is the generic `SHADER_RESOURCE` layout that
  initial data leaves textures in and direct and compute queues accept; `DepthStencilRead` is also
  `SHADER_RESOURCE`, since the depth-stencil-read layout admits no shader access; the rest map one
  to one. A barrier from `Undefined` names `LAYOUT_UNDEFINED` and `NO_ACCESS` before, and carries
  the discard flag, which initializes the compression metadata of a render-target, depth-stencil
  or storage texture; it is legal from any layout. A texture without initial data is created in
  `LAYOUT_COMMON`, the one layout a copy queue accesses, and one with initial data starts in
  `COPY_DEST`.
- **Ranges.** The mip and layer ranges resolve against the texture's description; a 3D texture is
  one array slice; a format with a stencil plane names planes 0 and 1.
- **Copy lists.** A copy queue takes no layout barrier and its textures stay in `General`, so a
  texture barrier on a Copy list, which keeps `General` on both sides (`VU-rojo-sync-0026`),
  becomes a global barrier of the same stages and accesses.

The backend tracks no layouts; the caller's `oldLayout` is the native `LayoutBefore`, as the
reference makes the caller state it. Three transitions are hidden from the caller: the clear
bracket above, the stager's move of initial-data textures to `SHADER_RESOURCE` (see "Initial data"
in the backend page) and, with render passes, a pass's own resolve transitions.

## Scopes and timestamps

`beginScope` calls the core, then `BeginEvent` (legacy Unicode metadata: the label as NUL-terminated
UTF-16 in the list's scratch) and `EndQuery` of a timestamp at the even index of a claimed pair;
`endScope` writes the odd index, then `EndEvent`. Each list owns its `ListTimestamps`: query heaps
of 256 queries (`TIMESTAMP`; `COPY_QUEUE_TIMESTAMP` on a Copy list), each with a committed
`READBACK` buffer of 2,048 bytes that is mapped once. The next heap and buffer are made when a pair
needs one (two `nativeCreateCalls`, one `gpuAllocations`, one `nativeHostCalls` for the map; no
lock), kept across recordings and named after the list.

A scope's record stores `(ordinal << 16) | pair`. A scope is untimed (`core::kNoTimestamp`) on a
device created without `DeviceDesc::scopeTimings`, whose lists make no heap or readback buffer and
resolve nothing, after the list has claimed 2^16 queries, for a pool ordinal above 0xFFFF, and on a
Copy list of a device with no copy-queue timestamps (`OPTIONS3`); an untimed scope records its event
pair only (1 call each end). `end` records one `ResolveQueryData` per heap the recording used, over
the used range, into that heap's readback buffer.

The `scopeTimes` hook runs at the frame wait, after the slot's work completed. For each record it
finds the list through `lists.at(slot, queue, ordinal)` (the pool mutex: a retire hook, never
recording), reads the two ticks from the mapped buffer and reports
`(end - begin) * 1000 / frequency` milliseconds, 0 when the end precedes the begin. The frequency of
each queue is read by `GetTimestampFrequency` at device creation. A retired frame with untimed
scopes logs one warning naming the reason; the frame's other queues log none.

## Counting

Counts follow the native call: every call on a native list is `core::noteNativeCall` once after
`beginList`, every `Barrier` adds one `nativeBarriers`, every `Create*View` one `descriptorWrites`.
Objects `createDevice` makes for itself are not counted. The operations count as follows.

| Operation | `nativeRecordCalls` | Other |
|---|---|---|
| acquire Graphics, Compute, Copy | 4, 3, 1 | slot reuse: 1 `nativeHostCalls` per list |
| `end` | 1, plus one `ResolveQueryData` per heap used | |
| `setRoot`, `pushRoot` | 0 | |
| `copyBuffer` | 1 | |
| buffer and texture copies | 1 per layer, 1 for a 3D region | |
| `fillBuffer` | 4; 2 when the kernel is already bound; +2 per later window | +1 `descriptorWrites` per later window |
| `barrier` | 1, or 0 for an empty batch | 1 `nativeBarriers` per call |
| `clearTexture`, render target or depth | 2 + one clear per mip and layer or slice | 2 `nativeBarriers` |
| `clearTexture`, Storage | 2 + one clear per mip | 2 `nativeBarriers`, 2 `descriptorWrites` per mip |
| `beginScope`, `endScope` | 2, 2 (1, 1 untimed) | |
| `Queue::submit` with lists | | 1 `nativeSubmitCalls` beyond the waits and signals |

A fill's minimum is the kernel form. Every other row is the native minimum.

## Threading and locks

Recording reads only what is fixed while a record lives: texture and buffer payloads through
`pool<T>().native(handle)` (one acquire load) and the directory through `HeapDirectory::find`. The
transient ring claims with one relaxed atomic add. A list's allocator, native list, binding,
barrier scratch and query heaps are its own, grown on the recording thread without a lock. A
lookup that only feeds a Debug check compiles out of Release. The one lock recording can take is
the frame-arena claim of `uploadFrameData` or a debug scope's label ([ADR 0020](
../decisions/0020-recording-locks-scope-labels.md)).

The list-pool mutex is a leaf taken by acquisition (including growth), by the slot reset and by
the scope hook. The lock order of the rest is in
[the backend page](d3d12-backend.md#threading-and-lock-order).

## Tests

`Tests/D3D12/` holds the `[d3d12]` cases; they pin counts and native forms through the core's
counters and `DeviceImpl`, read bytes back through native copies on a queue of their own
(`Tests/Support/D3D12/`), and submit what they record.

- `D3D12CommandListTests.cpp`: acquisition per queue type, `end`, one `ExecuteCommandLists` for
  three lists and none for none, allocator reset at slot reuse, label changes, four threads,
  `setRoot` and `pushRoot`, and the texture payload.
- `D3D12CopyTests.cpp`: call counts per layer and for 3D, bytes landed per subresource,
  `rowsPerImage` placement, a depth copy, sRGB to linear, and a Copy list's copy.
- `D3D12FillTests.cpp`: the pipeline, the four recorded calls and the skipped rebind, the dirty
  root, windows past 512 MiB, dispatch group counts and pattern byte order.
- `D3D12ClearTests.cpp`: every path and queue, 3D, stencil, multisampled and placed targets, the
  range written and nothing else, `CopyDst` afterwards, two threads and a clear after a fill.
- `D3D12BarrierTests.cpp`: one native call per batch, each lowering above, layout walks, planes,
  ranges, discard, initial data without a layer message, and the queue-masked accesses.
- `D3D12FillReachTests.cpp`: the bytes a fill writes across the 512 MiB window boundary.
- `D3D12ScopeTests.cpp`: event and timestamp calls, resolves, nesting, reuse, labels, threads,
  untimed Copy lists and the 2^16-query limit with its warning.
- `D3D12ScopeDurationTests.cpp`: four scope-duration cases, which cover what
  `ScopeConformanceTests.cpp` would for scopes that need no render pass, draw or upscaler.

Every `Tests/Conformance` and `Tests/Contract` file runs on this backend, except the few that
`xmake/targets.lua` leaves out; [the edges page](d3d12-backend-edges.md#the-t1-run-on-warp) names
them and the skips. That includes the copy, initial-data, barrier, fill, clear and handoff files,
the contract files for frame joins and queue hazards, `CopyTests` and `ScopeConformanceTests.cpp`;
[drawing](d3d12-drawing.md#tests) lists the rest.

All of this is WARP evidence under the debug layer and GPU-based validation. It is not hardware
evidence and not T1 acceptance, which needs a hardware run ([ADR 0009](
../decisions/0009-conformance-and-valid-usage.md)).
