# Drawing on the Metal 4 Backend

**Status**: Accepted

This page describes how the Metal 4 backend loads shader libraries, creates and caches pipelines,
opens render passes, binds state, draws, dispatches and runs counted and mesh draws. [The Metal 4
backend](metal4-backend.md) describes the device around it and
[command recording](metal4-recording.md) the lists, the open encoder, barriers and counts this page
builds on. Chapters 05 to 09 and 14 of the API reference stay the authority on semantics and on each
entry's Metal 4 mapping; every check lives in `Source/Core`, so no `ROJORHI_VU` appears under
`Backends/Metal4/`. A recording entry reads native objects through `HandlePool::native` and the
list's own state, never a pool record (ADR 0020), so Release recording takes no lock beyond a
frame-arena page claim.

## Shader libraries

A library is the RojoRHI container that `Tools/shader_library.py` writes around a metallib: text
lines `rojorhi.metal4.shaderlibrary`, `abi <version>`, one `entry <name> <stage> <x> <y> <z>` line
for each entry point (the stage and the threadgroup size the shader declares), a
`metallib <bytes>` line, then the payload. `metal4::parseShaderLibrary` is a pure function that
reads the lines in the order the reference gives and stops at the `metallib` line. Another magic,
a bare metallib and a blob of the Null backend included, is `DriverFailure` ("not built for the
Metal 4 backend"); another `abi` is `ShaderAbiMismatch`, decided before any entry line is read;
any other breach is `DriverFailure` naming the line.

`Device::loadShaderLibrary` runs the core's checks, parses, then inside an autorelease pool copies
the payload into a `dispatch_data_t` with the default destructor and calls
`MTL::Device::newLibrary`. It then checks every entry with `MTL::Library::newFunction` and
`MTL::Function::functionType`: a missing entry, or one of another stage than its line states, is
`DriverFailure` naming it, because Metal aborts a pipeline built on one. A refusal by Metal is a
`DriverFailure` with Metal's message, and every failure releases what the call made. The payload,
`metal4::LibraryPayload`, holds the `MTL::Library` and the threadgroup size of each entry in the
order of `ShaderLibraryRecord::entries`; the `shaderLibrary` retire hook releases it.

## Pipelines

Both `createPipeline` overloads read the library payload through `pool<ShaderLibrary>().native`,
find each entry's index under the pool's own `update`, and describe the pipeline with
`MTL4::RenderPipelineDescriptor`, `MeshRenderPipelineDescriptor` or `ComputePipelineDescriptor`
over `MTL4::LibraryFunctionDescriptor`s. The device's `MTL4::Compiler` compiles it, and the time
adds to `pipelineCompileNanoseconds`. The pipeline state carries the following.

- Every graphics pipeline supports indirect command buffers, which costs nothing, so a counted
  draw can execute under any of them.
- The threadgroup sizes of the library go to `setRequiredThreadsPerThreadgroup` for a compute
  entry and to the object and mesh forms for a mesh pipeline, and into the payload, which draws
  and dispatches read. The object size is set only with a task entry.
- A graphics pipeline with neither a fragment entry nor a depth format is compiled with
  rasterization off, since nothing it rasterizes is observable. Metal refuses rasterization off for
  a vertex entry that returns a value and on for a void one, and the container carries no return
  type, so a vertex pipeline with no fragment entry that Metal refuses is compiled once more with
  the other setting, and both refusals are reported if both fail. The retry costs creation time
  only. A mesh pipeline with color targets, a depth format and no fragment entry is refused with
  `DriverFailure`, because the validation layer aborts on it.
- Depth-stencil states come from a device cache, `metal4::DepthStencilCache`, keyed by the fields
  Metal receives, so equal states share one `MTL::DepthStencilState` and a bind filters by
  pointer. A pipeline that tests neither depth nor stencil holds none, Metal's default, and binds
  the cache's untested state, made at device creation. Only a creation takes the cache's mutex.
- `FrontFace::CounterClockwise` is `MTL::WindingCounterClockwise`. The payload also keeps the cull
  mode, fill mode, depth clip, whether the recorded depth bias applies, the list form of the
  topology class and `setsWinding` (below).

`metal4::PipelineState` is the payload; the retire hook releases the render or compute state and
leaves the shared depth-stencil state to the cache, which the device releases at teardown after
every pipeline retired.

## The pipeline cache

`PipelineCache` (`pipelineCacheData` and `DeviceDesc::pipelineCache`) is a RojoRHI text header
around a Metal 4 archive. The header is the lines `rojorhi.metal4.pipelinecache`, `version 1`,
`rojorhi <major>.<minor>.<patch>`, `device <name>` and `os <version string>`, then one section
per archive written: an `archive <bytes>` line and that many bytes. Flushing the serializer empties
it, so the device keeps every archive it has written and a blob holds them all, the newest last; a
call with no new pipeline returns the same blob. A blob whose header differs in any line from what this device, OS and
library would write, or whose byte count is not what remains, is one warning and a cache miss for
every pipeline; so is a blob of the Null backend, garbage, or an archive Metal refuses.

- The compiler descriptor gets an `MTL4::PipelineDataSetSerializer` that captures binaries, so a
  blob written later covers every pipeline created, hits included, as chapter 06 requires.
- A blob loads in `createDevice`. Each archive's bytes go to its own temporary file in the per-user
  directory `confstr(_CS_DARWIN_USER_TEMP_DIR)` returns: RojoRHI reads no environment variable,
  and `std::filesystem::temp_directory_path` would read `TMPDIR`. `MTL::Device::newBinaryArchive`
  checks every file first, since a damaged archive with a valid header makes the driver throw at the
  first compile, then `MTL::Device::newArchive` opens it, and a creation probes each archive. The files stay, read-only, until
  `releaseNative` removes it, because deleting it makes later compiles throw in the driver; a
  process that crashes leaves its files behind. A compile finds a file gone with one warning and
  drops the archive lookup from then on. Each archive costs one `access` and one probe per creation,
  and the loaded archives are labeled `rojorhi.pipelineCache.<n>`. A blob whose second section is
  malformed is discarded whole, each form (a count that does not fit, bytes after the sections, a
  damaged archive) with its own warning, and the files already made are removed.
- A creation probes the archive with `MTL4::Archive::newRenderPipelineState` or
  `newComputePipelineState`. A state is a hit, counts in `DeviceCounts::pipelineCacheHits` once
  the pipeline exists and is released; the pipeline is then always compiled through the compiler
  with `MTL4::CompilerTaskOptions::setLookupArchives`, which serves it from the archive on a hit.
  The serializer therefore sees every pipeline. `counters()` reports misses as the pipelines
  created minus the hits.
- `pipelineCacheData` serializes under the cache's own lock, a `std::shared_mutex` it holds
  exclusively, and opens its own pool. Every compilation through the device's compiler after
  `createDevice`, a pipeline's or a MetalFX scaler's, holds the lock shared, so compilations still
  overlap one another but never a flush: Metal crashes in
  `-[_MTLBinaryArchive newArchiverIdWithBinaryKey:...]` when the serializer is flushed while a
  compilation records into it. A pipeline is therefore in the archive of one harvest or the next,
  never lost between them. A device that created no pipeline returns a blob a later device accepts,
  about 170 KB, since the backend's own kernel pipelines are compiled on the same compiler in
  `createDevice`, before any harvest. Those kernels use no archive lookup and count no hit.

The Null backend never counts a hit. A hit costs about 0.5 ms and a miss about 8 ms with the
validation layer off; under `MTL_DEBUG_LAYER=1` the debug compiler serves repeat compiles itself
and a hit costs more than a miss.

## Render passes

`beginRenderPass` runs the core's check, closes the open encoder, pushes the pass label as a debug
group on the command buffer (the group joins the stack that names encoders), copies the attachments
into list-owned storage, reading native textures through `pool<Texture>().native`, and opens no
encoder. The render encoder opens at the first draw that is not a counted draw, at a scope boundary
inside the pass or at `endRenderPass`, from one `MTL4::RenderPassDescriptor` that the list allocates
at its first pass and reuses, with its attachment descriptors, which the list keeps retained, so
configuring them and emptying them at `end` call no getter and need no pool.

- The first encoder of a pass takes the caller's load actions and clear values; an encoder that
  resumes after a split takes `Load`. Slots beyond the pass's attachments are nulled. A value an
  attachment descriptor holds (`DrawState::colorHeld`, `depthHeld`, `stencilHeld`: textures it
  retains, clear values bit for bit) is not set again, about 70 ns a pass (C1, K 1, M3 Max).
- A store action that is `Store` without a resolve is set on the descriptor at once, which costs no
  call. Every other one is `StoreActionUnknown` on the descriptor and is set on the encoder before
  `endEncoding`: the caller's action, or its resolve form, at `endRenderPass`, and `Store` at a
  split. A split stores and resolves an attachment that has a resolve target, because the validation
  layer rejects a plain `Store` there; the split costs one extra resolve.
- The encoder is labelled after the innermost open scope when the device labels encoders, binds the
list's argument table to the vertex and fragment stages (ADR 0018), emits the render part of the
pending barrier, and replays the prefix (next section). Its first mesh pipeline binds the table to
the object and mesh stages too: each bound stage costs every draw that follows a table write
(about 2 ns per draw for the two mesh stages on C1 and C2, Release, Apple M3 Max).
- `beginRenderPass` claims the pass's timestamp pair (none without `DeviceDesc::scopeTimings`) and
  keeps its begin entry in `DrawState::passBegin`; the pass's first encoder writes it with
  `writePassBegin` right after it opens, before any command: the compute encoder of the pass's
  leading counted draws, when their kernels run first, else the render encoder, after
  `RenderStageVertex`. `endRenderPass` opens the encoder when nothing did, sets the deferred store
  actions, writes the end timestamp on it after `RenderStageFragment` and pops the pass's debug
  group; the encoder then lingers, running no further command and hidden from the draw state, until
  the next encoder, a scope boundary or `end` ends it, so a barrier the list still owes at `end` can
  be carried on it ([command recording](metal4-recording.md#barriers)). Both are written at
  `TimestampGranularityPrecise`, which adds no encoder at an encoder's start or end; the kernels of
  counted draws and the encoders splits resume fall between the two. A command buffer's
  `writeTimestampIntoHeap` around a render encoder costs CPU and allocations per write and slows
  that encoder's draws on the GPU, so no pass writes one. `end` nulls the textures the descriptor
  holds, so an idle list keeps no destroyed target alive; descriptor setters are not counted.
- A scope that begins or ends inside a pass opens the render encoder and pushes or pops a debug
  group on it. A split pops the open groups first and the resumed encoder pushes them again,
  outermost first. Such a scope writes its timestamps on the render encoder after the fragment
  stage, at `TimestampGranularityPrecise`; a split writes none ([command
  recording](metal4-recording.md)).
- Ending a render encoder opens a command pool, once per encoder and never per command, because
  the validation layer autoreleases when a stencil attachment ends.

## State and the prefix

The list keeps the graphics state it recorded and what the open render encoder holds. Until that
encoder opens, `setPipeline` with a graphics pipeline, the five dynamic-state setters, `setRoot` and
a counted draw's execute append to a list-owned prefix, a vector that keeps its capacity. A later
entry of a kind replaces an earlier one that no execute separates from it, so a pass that binds two
pipelines and draws with neither sends only the last. The open replays the prefix in order through
the filtered setters, so each execute sees the state recorded before it.

- The held state starts at Metal's encoder defaults: no pipeline, no depth-stencil state,
  `CullModeNone`, `WindingClockwise`, `TriangleFillModeFill`, `DepthClipModeClip`, bias 0, stencil
  reference 0, blend color 0, viewport and scissor unknown. A setter calls Metal only for a value
  that differs, so a depth-stencil state is compared by pointer; null is sent as the untested
  state, which Metal binds about 1 ns faster (C3, Apple M3 Max).
- `setPipeline` keeps the last two payloads it resolved in the recording, by handle, and reads a
  handle bound again from them; another is read through the native table and replaces the older. A
  handle's bits name one allocation (a release advances the generation) and a payload retires after
  the list's frame, so a live handle's entry is its payload. A hit reads no table: a list still
  recording after its frame retired (`VU-rojo-device-0005`, checked at `endFrame` in Debug) can read
  a retired one. Acquisition empties them.
- The winding decides front and back faces even with culling off, since two-sided stencil and
  `SV_IsFrontFace` read it. A pipeline's winding is applied, filtered, when it culls, tests stencil
  or has a fragment entry (`setsWinding`); a depth-only or rasterization-off pipeline skips it.
- The applied depth bias is the recorded values when the bound pipeline has `depthBias`, else
  zero. Topology is list state only: `setPrimitiveTopology` sets no native state, `setPipeline`
  resets it to the list form of the pipeline's class, and each draw passes it.
- `setRoot` writes the table's `buffer(0)` at once outside a pass or with the render encoder open,
  filtered, and joins the prefix otherwise. Slots 1 and 2 hold the view and sampler tables.
- A direct draw passes base instance 0. An indexed draw passes the index address plus `firstIndex`
  times the index size and the bytes that remain. A direct mesh draw binds four zero bytes at
  `buffer(3)`, the `drawId` the shader reads, filtered; the list writes them into its frame-arena
  page at its first direct mesh draw of a recording.
- Every draw skips a zero count, marks the encoder as holding work, and records the stages it ran
  for the barrier lowering. Each native draw runs inside a `CommandPool`, since the validation
  layer autoreleases inside a draw; that is one pool per draw under the layer and none without it.
  The common draw, one instance and base vertex 0 with no pool, is sent in the entry; any other
  goes through an out-of-line helper, so link-time optimization inlines the entry into its caller.

## Compute binding and dispatches

`setPipeline` with a compute pipeline opens the lazy compute encoder and sets the pipeline there,
unless the encoder already holds it (chapter 07). The list keeps the caller's compute pipeline;
`Encoders::kernelPipeline` is the pipeline the open encoder holds, whether the caller's or a
backend kernel's. A kernel run leaves its own pipeline on the encoder, so `dispatch` and
`dispatchIndirect` set the caller's again when the two differ, which a new encoder and a kernel
both cause. Restoring at the next dispatch orders the same as restoring after every kernel and
spends a call only when a dispatch follows. A dispatch with a zero group count returns before any
encoder work. Both entries take the threadgroup size from the payload, run the native dispatch
inside a command pool and count one message; `dispatchIndirect` passes the GPU address of the
arguments. A pending barrier owed to an encoder that opened empty is emitted at the encoder's next
command of any compute stage.

## Counted draws

Every `drawIndirect` and `drawIndexedIndirect` is counted, as is a mesh draw with a count or more
than one record. With no native form that reads a GPU count, a backend kernel reads the count and
encodes the draws into an indirect command buffer (ICB) or, at `maxCount` 1, writes one native
record. Chapter 08 states the cost.

- **Hoisting.** While the render encoder is closed, the kernel goes on a compute encoder ahead of
  it, which the batch's kernels share and which holds only kernels: `bindKernelBatch` sets the
  pipeline when it differs and the kernel table once, and no list table is set back. The draw's
  execute joins the prefix, after the state recorded before it.
- **The owed barrier.** A backend barrier from `StageDispatch` to `StageVertex` with
  `VisibilityOptionDevice` joins the pending barrier through `BarrierState::merge`, and the render
  encoder emits it when it opens. Metal reads the ICB, the execution range and the native records
  before vertex shading; naming the object and mesh stages for a plain draw would hand them back as
  unrun at every close, so the mesh kinds add `StageObject | StageMesh`. A caller's `IndirectArgs`
  barrier reaches the kernel's dispatch stage when the compute encoder opens.
- **The split.** A counted draw recorded after the render encoder opened splits the pass first: the
  open scope groups are popped, every attachment gets its store action, the encoder ends and the
  pass is marked resuming. The next open loads, replays the held state, pushes the groups again and
  replays the prefix. A split also orders the closed encoder's fragment stores before the resumed
  encoder's loads, with `StageFragment` as source and destination merged into the barrier the
  resumed open emits. It costs a store and a load per attachment, one more resolve per resolving
  attachment, and the batch's compute encoder.
- **ICB pools.** Per list and per kind (draw, indexed, mesh) the list keeps a vector of slots and a
  cursor that resets at acquisition. Slot `i` serves the `i`-th counted draw of that kind in a
  recording and is reused while its capacity reaches `maxCount`; otherwise it is replaced by one of
  `bit_ceil(max(maxCount, 64))` commands, since the list is acquired again only after its frame
  slot's work retired. A draw command costs about 670 bytes and a mesh command 1,536, measured on an
  Apple M3 Max. The descriptor inherits the pipeline state and every `inherit*` state, and inherits
  the buffers for draw and indexed kinds. The slots outlive a recording, so a list that records the
  same counted draws again makes no native object.
- **Residency.** The ICBs are resident through the device's residency set, which every queue keeps.
  Recording takes no lock (ADR 0020), so the list only notes the ICBs it made and replaced;
  `Queue::submit` adds the new ones to the device's set and removes and releases the replaced ones,
  under the set's mutex, before it commits the set: an ICB joins the set before the command buffer
  that uses it commits, and a replaced one leaves at the list's next submission, after its last use
  retired. A list recording the same counted draws again hands over nothing. Teardown removes the
  ICBs from the set and releases them.
- **The kernels.** `Backends/Metal4/Kernels/Metal4DrawIndirect.metal` holds six functions,
  `rojorhi_encode_draw`, `rojorhi_encode_indexed`, `rojorhi_encode_mesh`, `rojorhi_write_draw`,
  `rojorhi_write_indexed` and `rojorhi_write_mesh`, whose pipelines the device makes with the fill
  and clear ones. Topology and index type are run-time parameters, since they vary per call. The
  encode kernels run one thread per command in groups of 64, encode the record when it lies below
  the count and reset the command otherwise (a record with no vertices or instances is an empty
  command), and write the execution range. A writer, one thread per draw at `maxCount` 1, writes a
  native record, with no instances when the count is 0.
- **The parameters.** One 96-byte reservation in the list's frame-arena page, claimed through
  `core::bumpArenaPage` and counted as no frame data, holds the kernel's `DrawParams` (64 bytes) and
  either the ICB's resource ID and the execution range or the writer's native record. A counted
  mesh draw reserves 128 bytes: the kernel's `MeshParams` (96 bytes), which also carries the root
  recorded with the draw and the addresses of the view and sampler tables that each mesh command
  bakes in, then the same ICB ID and range or native record. Replay runs
  `executeCommandsInBuffer(icb, rangeAddress)` or the native indirect draw of the record.
- **Counts.** An encode kernel costs two native messages (address, dispatch) once its pipeline is
  set, three when the kind changes, four on a new encoder, and its execute one at replay. Writers of
  consecutive draws of one kind, reserved back to back, form one run (`Encoders::queueKernel`) that
  the next encode kernel or the encoder's close dispatches at those costs, a thread per draw, so a
  writer's draw costs only its replay. The batch adds its encoder's open and end, a barrier at the
  render open and a timed pass's begin timestamp. Dispatches and executes each run in a pool.

## Mesh paths

A direct `drawMeshTasks` binds the zero record at `buffer(3)` and calls `drawMeshThreadgroups`
with the pipeline's object and mesh threadgroup sizes. A mesh draw with a null count and a lone
record is not counted: it binds the record's address at `buffer(3)`, so the shader's `drawId`
reads the record's first word, and draws over the three threadgroup counts that follow it at the
record's address plus four, opening the render encoder like any direct draw. Any other mesh
indirect draw is counted: its kernels are the batch form of the previous section, its ICB kind is
the mesh one, and the barrier adds the object and mesh stages. A mesh ICB command binds buffers 0
to 3 on the object, mesh and fragment stages, with four bind counts and `inheritBuffers` false:
nothing reaches an ICB command from the table, and Slang gives every entry of a file each reserved
binding any entry of the file touches. The writer path binds the record at `buffer(3)` to draw.

Slang drops the user semantic of mesh vertex outputs when it compiles to MSL, so Metal refuses a
mesh pipeline whose fragment entry reads a vertex varying; a mesh shader passes its values per
primitive until the toolchain keeps the semantic.

## Tests

`Tests/Metal4/` holds the `[metal4]` cases of this page: library parsing and loading, pipeline
creation and its refusals, the cache and its refusals, render passes and their store actions,
graphics state and its filtered native calls, dispatches, counted draws (kernel and encoder order,
call counts, the split, ICB reuse and residency), mesh indirect draws, how barrier batches lower
onto the encoders, and the call counts of the drawing paths. `Tests/Conformance/` holds T1 oracle
cases under API validation: render targets by format and load and store action, draws with exact
images, depth, stencil, culling and bias, the ABI vertex and fragment shaders run through direct and
indirect draws, the ABI compute and texture kernels, dispatches, counted draws at 1 to 65,536
records with every count form, mesh tasks direct and indirect, and barriers across passes,
dispatches, copies, counted draws, lists and queues. A kernel-encoded ICB command runs only under
API validation: shader validation silently drops it. [Testing](../conventions/testing.md) owns the
tiers and the oracle shaders.
