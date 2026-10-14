# Command Recording on the Null Backend

**Status**: Accepted

This page continues [the Null backend and the shared core](null-backend.md) with the recording
chapters: command lists, frame data, shader libraries, pipelines, render passes, draws, copies and
barriers. The same layering holds. Every check and every piece of recording state lives in
`Source/Core`; `Backends/Null/Source` translates and contains no `ROJORHI_VU`. The API reference
stays the authority on semantics.

## Files

| Area | Core (`Source/Core/`) | Null backend (`Backends/Null/Source/`) |
|---|---|---|
| Command lists | `CommandLists.*` | `NullCommandList.cpp`, `NullDevice.h` |
| Frame data | `FrameArena.*`, `CommandLists.*` | `NullCommandList.cpp` |
| Addresses | `Addresses.*` | `NullAddr.cpp` |
| Libraries and pipelines | `Pipelines.*` | `NullShaderLibrary.*`, `NullDevice.cpp` |
| Passes, scopes, state | `RenderPasses.*` | `NullCommandList.cpp` |
| Draws and dispatches | `Draws.*` | `NullCommandList.cpp` |
| Copies and clears | `Copies.*` | `NullCommandList.cpp` |
| Barriers | `Barriers.*` | `NullCommandList.cpp` |

## Command lists

A `CommandList` is an object handle: a pointer to the backend's `CommandListImpl`, which embeds a
core `CommandListCore`. The core struct is the state machine every recording rule reads: the
device, queue type and frame the list was acquired in, a `ListState` (Recording, Ended, Submitted),
whether a render pass is open and its `PassState` (attachment formats, sample count, area, whether
a viewport and scissor were set), the bound pipeline, the root address, one entry per open scope
(true when it began inside a pass), the label and the frame-data page. Commands are not stored:
the Null backend renders nothing, and on a validating device the list also owns the tracker log
of [the validation tracker](validation-tracker.md).

Recording follows [ADR 0020](../decisions/0020-recording-locks-scope-labels.md). Every lookup of a
handle's or an address's record, and every loop, whose only purpose is a Debug-tier check sits under
`#ifndef NDEBUG` inside its entry, so a Release build reads no pool while recording and takes no
lock except to claim a frame-arena page. In Release, `PassState` therefore holds no attachment
formats or sample count and the bound pipeline is not recorded; nothing but the checks reads them.

`Device::acquireCommandList` checks that a frame is open (`VU-rojo-cmd-0001`) and takes a list from
the pool of the open frame's slot. There is one pool per slot, each with its own mutex, because
acquisition is free-threaded. A pool grows when the frame needs more lists than the slot ever had
and keeps its lists; when `beginFrame` reuses the slot, the `frameSlot` hook sets the pool's
`used` count back to zero, so every list is acquired again. `beginList` resets every field of the
core state while keeping the capacity of its containers, checks the label and counts the list as
unsubmitted. `Device::endFrame` requires that count to be zero (`VU-rojo-device-0005`).

`CommandList::end` requires a recording list with no pass and no scope open
(`VU-rojo-cmd-0002` to `0004`). `submitLists` runs first in `Queue::submit`: for each list it
checks the default handle (`VU-rojo-core-0004`), the device (`VU-rojo-core-0005`), the queue type
(`VU-rojo-queue-0002`) and that the list ended, belongs to the open frame and was not submitted
before (`VU-rojo-queue-0001`), then marks it submitted.

Every other command calls `checkRecording` first, the one site of `VU-rojo-cmd-0063`: the list has
not ended. `uploadFrameData` checks its own arguments and the open frame (`VU-rojo-buffer-0015` to
`0017`) before it. A list handle is valid until its frame retires; later use is undefined behaviour
that no tier detects. The handle is a pointer, not a generation-checked handle: until the slot's
pool is reused, a list of a retired frame shows as a Submitted list, which `VU-rojo-cmd-0063`
reports; once the same object is acquired again, a stale copy of the handle records on the new list
without a diagnosis.

## Frame data

`CommandList::uploadFrameData` copies into a page of the per-slot `FrameArena` and returns an
`Addr` that names the page. A page is a host buffer of the ordinary buffer pool, labelled
`rojorhi.frameArena.page` and created with `BufferUsage::IndirectArgs`, so lookups in the manner of
`Device::address` and `resolveAddr` treat it like any buffer, and an indirect command accepts
frame data as its arguments, as chapter 03 allows. The arena has one list of pages per frame slot,
each with a `claimed` flag.

A list claims a whole page and bumps its own cursor (`CommandListCore::pageUsed`) with no lock.
Claiming takes the arena's mutex, and creating a page, when no unclaimed page of the slot fits, is
a growth event. A new page has max(1 MiB, request) bytes, falling back to exactly the request only
when that maximum exceeds `Limits::maxBufferSize`. A request above `maxBufferSize`, or a page the
backend cannot create, is fatal in every build, as the reference says. `rewind(slot)`, called from
`beginFrame` when the slot is reused, makes every page claimable again; pages keep their memory.
Claiming is the one place Release recording takes locks (ADR 0020): the arena's mutex, the buffer
pool's inside it when a page is created, and the buffer pool's again afterwards to stamp the page's
`BufferRecord::arenaFrame` with the claiming list's frame. A scope label claims through the same
path as an upload ([the edges](null-backend-edges.md) describe it).

`VU-rojo-buffer-0020` (an address of frame data is passed only to a list of the frame that produced
it) is checked in `resolveAddr`, which compares the list's frame with the page's `arenaFrame`; no
page is scanned. It replaces the retired `VU-rojo-buffer-0018`. The Debug tier sees only the
commands that take the address: not an address reached through root data or other buffer memory,
not a shader's read, and not a stale address into a page that a list of the later frame has
claimed again.

## Address resolution

`resolveAddr(core, list, addr)` is the single core site of `VU-rojo-buffer-0009` (the address
names a byte inside a live buffer) and of `VU-rojo-buffer-0020`. Its callers are the commands that
take an `Addr`: `setRoot` (and so `pushRoot`) and the four indirect commands. Indexed draws and
copies take `Buffer` handles and look them up instead. It returns the buffer handle, record and
byte offset, or a null result for the null address; the caller decides whether null is allowed. It
reads the buffer pool under its lock, so its callers call it in Debug builds only.

The backend supplies `decodeAddr`, defined next to `coreOf`. The Null decode is the encoding of
the buffers section: the slot index in `lo`, the byte offset in `hi`. It ignores the generation,
because an `Addr` carries none. A destroyed buffer whose slot was reused resolves to the new
buffer; `VU-rojo-buffer-0009` catches an address into a slot that is free or whose record is
smaller than the offset, and nothing catches the reuse. That is the limit of the Debug tier here;
the tracker cannot recover it either, since the address holds no generation.

## Shader libraries

A Null library is a text blob, the format `parseNullShaderLibrary` reads:

```
rojorhi.null.shaderlibrary
abi 1
entry vsMain vertex
entry fsMain fragment
```

The first line is the magic, the second the shader ABI version (`kShaderAbiVersion`, equal to
`ROJORHI_SHADER_ABI_VERSION` of `RojoRHI.slang`), then one `entry <name> <stage>` line per entry
point, with the stage one of vertex, fragment, task, mesh or compute. `Device::loadShaderLibrary`
first runs `prepareShaderLibrary` (an empty blob, the label rules), then parses. Failures are
results, never violations:

- another magic: `ErrorCategory::DriverFailure`, the library was not built for this backend;
- another ABI version: `ErrorCategory::ShaderAbiMismatch`;
- a malformed second line, an entry line that is not `entry <name> <stage>` or a repeated entry
  name: `DriverFailure` with the line number.

The entries go into the `ShaderLibraryRecord`, which pipeline creation checks against. Tests build
blobs in code with `Tests/Support/NullLibrary.h`.

## Pipelines and the pipeline cache

`prepareGraphicsPipeline` and `prepareComputePipeline` check a description: the entries exist in
live libraries with the right stage, exactly one of a vertex and a mesh entry (and a task entry only
with a mesh), conservative rasterisation only where the device reports it, the color target count,
and a depth format that has the components the depth and stencil tests need. `VU-rojo-format-0002`
(no `Undefined` color target) and `VU-rojo-format-0003`, which owns every format capability, are
checked at the pipeline site as well as at resource creation: a color target's attachment, blend
and sample-count support and the depth format's attachment and sample-count support, five sites of
`VU-rojo-format-0003`, each with its death test. The record keeps what later checks compare
against: the kind, stage bits, topology class, color formats, depth format, sample count and
depth-bias flag. A Null pipeline owns no native object.

The cache is a count. Each created pipeline raises `cachedPipelines`. `Device::pipelineCacheData`
returns a text blob:

```
rojorhi.null.pipelinecache
version 1
pipelines 3
```

`createDevice` reads `DeviceDesc::pipelineCache`: a blob this backend wrote seeds the count, and
any other (foreign magic, another version, wrong shape) is ignored with a warning, as chapter 06
says, so a round trip through `pipelineCacheData` and `createDevice` preserves the number. The
reference leaves the bytes' lifetime as a requirement on the caller (it is not a rule).

## Passes, scopes and dynamic state

`beginRenderPass` checks the description against the list (a graphics-capable list, no pass
open, attachments of live textures with usage, format, extent, sample and resolve rules, and the
resolve capability under `VU-rojo-format-0003`) and opens the pass: it copies the formats, sample
count and area into `PassState`, clears the dynamic state flags and unbinds the pipeline.
`endRenderPass` requires an open pass and that every scope begun inside it has ended
(`passScopeBase` marks where the pass began), then unbinds. Scopes nest across the pass boundary
but must end on the side they began.

`setPipeline` binds a graphics pipeline inside a pass when its formats and sample count match the
pass, or a compute pipeline outside a pass on a list that supports compute. The dynamic-state
setters (`setViewport`, `setScissor`, `setStencilReference`, `setBlendConstants`,
`setPrimitiveTopology`, `setDepthBias`) check the bound pipeline and their arguments; viewport and
scissor are recorded as set, which draws require. Values are not stored.

## Draws, dispatches and indirect commands

`setRoot` checks that the address is not null, names a live buffer and has an offset that is a
multiple of 256, the number `VU-rojo-abi-0002` states (`VU-rojo-abi-0001`, `0002`). `pushRoot` is
a template over `uploadFrameData` and `setRoot`, so it adds no check of its own. A draw asks one
shared problem function whether a pass is open and a graphics pipeline with the stage the draw
needs is bound, and checks that a viewport and scissor were set since the pass began; the indexed
draw adds the index buffer, type and range. The indirect draws check the same three things under
their own identifiers (`VU-rojo-indirect-0001`, `0006`, `0012` and `0021` to `0023`). A dispatch
needs no pass, a list that is not a Copy list and a bound compute pipeline. The indirect forms
resolve the argument address through `resolveAddr` and check that it is 4-byte aligned, in a
buffer created with `IndirectArgs` and large enough for the arguments (chapter 08). The Null
backend then does nothing.

## Copies and clears

Copy, fill and clear entries check that no pass is open, look up each buffer and texture, check
the buffer roles of `VU-rojo-buffer-0001` and `0002` (once, for every copy), the texture region
and subresource rules (`VU-rojo-cmd-0038` to `0041`, `VU-rojo-texture-0016`, `0017`, `0041`),
format compatibility, a single-sampled texture in a buffer-texture copy (`VU-rojo-cmd-0066`,
`0068`) and that the bytes `layout` addresses lie inside the buffer (`VU-rojo-cmd-0064`, `0065`).
The same range rules serve views, copies, clears and barriers.

## Barriers

`barrier` checks the Debug-tier rules of a batch: stage and access sets valid for the list's queue
type, a buffer range inside its buffer and a texture's layouts and subresource range, the live
resource lookups, and no barrier inside a pass (`VU-rojo-sync-0019`). Rules that follow a resource's
state across commands are the validation tracker's. On a validating device each record is logged
and replayed at submission, which judges the layouts, the hazards, aliasing and the references of
unsubmitted lists ([the tracker](validation-tracker.md)). The Null backend discards the
barriers.

## Tests

`Tests/Contract/` has a `*Tests.cpp` and a `*DeathTests.cpp` for each of command lists, pipelines,
render passes, draws, copies and barriers. `Tests/Core/` pins the list state machine, the address
decode and the pipeline records through `core::coreOf`; `Tests/Null/` the library and cache
formats and the list pools. `Tests/Support/NullLibrary.*` builds library blobs and
`Tests/Support/PassKit.*` builds a device with an open frame, a library, a list, attachment
textures and pipelines.

## What remains

- A native backend: slot payloads for views and samplers, and the use of the retire and
  `frameSlot` hooks for native command lists and arena pages.
