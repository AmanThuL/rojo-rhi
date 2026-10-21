# Drawing on the D3D12 Backend

**Status**: Accepted

This page describes how the D3D12 backend loads shader libraries, creates pipelines and caches them,
and how it binds them, records passes, draws and dispatches. It is implemented, verified on WARP
only, not on hardware, and the backend is not a capability ([ADR
0009](../decisions/0009-conformance-and-valid-usage.md),
[documentation](../conventions/documentation.md)). [The D3D12 backend](d3d12-backend.md) describes
the device around it and [recording](d3d12-recording.md) lists, copies, barriers and scopes. The API
reference stays the authority on semantics; every check lives in `Source/Core`, so no `ROJORHI_VU`
appears under `Backends/D3D12/`. A recording entry reads native objects through `HandlePool::native`
and the list's own state, never a pool record (ADR 0020).

## Shader libraries

A library is the RojoRHI container that `Tools/shader_library.py --format d3d12` writes beside the
`.dxil` files of a shader, as `<Name>.d3d12lib`. It is ASCII lines, each ended by one LF, then
raw bytes:

```
rojorhi.d3d12.shaderlibrary
abi <N>
entry <name> <stage> <x> <y> <z> <bytes>      one line for each entry point
dxil <total>
<total raw bytes: each entry's DXIL container in entry order, to end of file>
```

The stage word is the spelling of `core::stageName`; the size is the `LocalSize` of the SPIR-V
module and `0 0 0` for vertex and fragment entries; `<total>` is the sum of the `<bytes>`. The
closing `dxil` line ends the text, so a reader never reads DXIL as text. The ABI stamp lives in this
container, not in the DXIL, which stays as the compiler signed it.

The writer checks every `--dxil NAME=PATH` file and refuses one that does not start `DXBC`,
whose size field (a little-endian `uint32` at byte 24) differs from the file's length, or whose
16-byte digest (bytes 4 to 19) is all zero, which is unsigned DXIL the runtime refuses, naming the
file. An entry's `.dxil` is the shader rule's own: `<name>.dxil`, or `<name>.<entry>.dxil` for a
shader with several entries.

`d3d12::parseShaderLibrary` is a pure function with no Windows header, and reads in this order:

1. The first line is not the magic (a bare DXIL container included): `DriverFailure`, "the
   shader library was not built for the D3D12 backend".
2. The second line is not `abi <number>`: `DriverFailure`; a number other than
   `core::kShaderAbiVersion`: `ShaderAbiMismatch` naming both, before any entry line is read.
3. Entry lines of seven tokens, then the `dxil <total>` line, the sum of the byte counts, the
   bytes that remain and the `DXBC` prefix of each entry's slice. Any breach is `DriverFailure`
   naming the line. Nothing reads past the end of the blob.

`Device::loadShaderLibrary` runs the core's checks, parses, and copies the DXIL region once into
`d3d12::LibraryPayload` together with the offset and size of each entry, in the order of
`ShaderLibraryRecord::entries`. The payload holds no native object: the runtime sees the DXIL
only when a pipeline is created from it. The `shaderLibrary` retire hook frees the payload with
the library's frame.

`ShaderList.h` carries `kD3D12LibrariesBuilt`, true when the shader rule found a working DXC and
so packaged every library. That holds on the `d3d12` CI job and on the `interface` job's Windows
leg, where the SDK's DXC loads; the D3D12 cases of the `abi` entry run there and skip with that
reason elsewhere.

## Pipelines

Graphics, mesh and compute pipelines take one path. `Device::createPipeline` builds a
`d3d12::PipelineStream` and hands its `D3D12_PIPELINE_STATE_STREAM_DESC` to the pipeline cache,
which creates the state with `ID3D12Device2::CreatePipelineState`, or takes it from the pipeline
library, so creation and the cache share the same stream. The stream is a byte buffer of 8-byte
words that lays each subobject out as the runtime reads it: the subobject type at a multiple of
the pointer size, the value at its own alignment. It is not a struct of `alignas` wrappers,
which MSVC refuses under `/WX` (C4324). The subobjects are:

- The global root signature, in every stream of every kind. Shaders carry none (ADR 0018).
- The stages: VS and an empty `INPUT_LAYOUT` for a vertex pipeline, AS (with a task entry) and
  MS for a mesh pipeline, which has no input layout, PS with a fragment entry, or CS. Each is a
  `D3D12_SHADER_BYTECODE` into the library payload, which a pipeline references only during
  creation.
- For a graphics or mesh pipeline: `BLEND` with `IndependentBlendEnable` true, `RASTERIZER2` with
  the bias fields zero, `DEPTH_STENCIL2`, `RENDER_TARGET_FORMATS`, `DEPTH_STENCIL_FORMAT`,
  `SAMPLE_DESC` of the sample count and quality 0 and `SAMPLE_MASK` all ones.
  `PRIMITIVE_TOPOLOGY` carries the type of the topology class for a vertex pipeline. A mesh
  pipeline's stream names the output topology its mesh entry declares, read from the `PSV0` part
  of the entry's DXIL, so the description a pipeline library stores is the one a later look-up
  passes; it names none only when the container declares none.
- `FLAGS` with `DYNAMIC_DEPTH_BIAS` when the raster state enables depth bias.

Line rasterization is aliased, there is no strip cut and no depth bounds test.

`d3d12::PipelineState` is the payload. It holds the pipeline state object (one reference), the kind
(graphics, mesh or compute), the list form of a vertex pipeline's topology class (undefined for the
other kinds) and whether depth bias is enabled. A bind reads these through
`pool<Pipeline>().native`, one load without a lock; `setPipeline` binds them (see Lazy binding). The
time of the creation, a cache hit included, adds to `pipelineCompileNanoseconds`, and the pipeline
is named from its label.

A refused native creation is `d3d12::failure`: `OutOfMemory` or `DriverFailure` from the
HRESULT, naming the call and the pipeline's label, with the removal report when the device was
removed. An entry name the library lacks, which only Release lets past the core, is `DriverFailure`.

## The pipeline cache

`pipelineCacheData` and `DeviceDesc::pipelineCache` carry a RojoRHI header around the bytes of an
`ID3D12PipelineLibrary1`. The blob is ASCII lines `rojorhi.d3d12.pipelinecache`, `version 1`,
`rojorhi <major>.<minor>.<patch>` and `library <bytes>`, then `ID3D12PipelineLibrary1::Serialize`
output. `createDevice` opens the cache from the blob:

- An empty blob makes an empty library and reports nothing.
- A header that differs (another backend, blob version or RojoRHI version, or a byte count that
  is not what remains), or library bytes `CreatePipelineLibrary` refuses (another adapter or
  driver, or bytes that are no library), is one `Warning` and an empty library. Creation still
  succeeds and every pipeline is a miss.
- A valid header with `library 0` makes an empty library without a message.
- The device keeps a copy of the library bytes until teardown, because the library reads them in
  place. They are released after every pipeline has retired.

If the runtime refuses even an empty library (`DXGI_ERROR_UNSUPPORTED`), the device has no
library: one `Warning`, every creation a miss, and the blob it returns carries `library 0`, which
a later device accepts.

The key is 128-bit FNV-1a (offset basis `0x6c62272e07bb014262b821756295c58d`, prime
2^88 + 0x13b, held as two `uint64_t` limbs) over canonical bytes: the kind, the stages in
declaration order, each with its entry name and the 16-byte digest of its DXIL container, and
every field of the description that reaches the stream, in a fixed order. The topology class is
part of the key for a vertex pipeline only, and the label is not. The name given to the library
is the key as 32 lowercase hex digits in a wide string. `LoadPipeline` compares the
stored description with the stream and refuses a different one with `E_INVALIDARG`. Without the
debug layer that is a miss like an absent name. With the layer on, the runtime's
`LOADPIPELINE_INVALIDDESC` error (message 972) is not filtered and aborts, because the key covers
the whole description and such a mismatch can only show a defect.

A creation calls `LoadPipeline`. A returned state is a hit, counted in `pipelineCacheHits` once the
pipeline is published. On `E_INVALIDARG` it calls `CreatePipelineState` and then `StorePipeline`,
whose `E_INVALIDARG` for a name that already exists is ignored; any other failure of `LoadPipeline`
or `CreatePipelineState` fails the creation, and any other failure of `StorePipeline` is one
`Warning` and leaves the pipeline uncached. The core derives `pipelineCacheMisses` as the pipelines
created minus the hits. Two threads that miss one key both compile.

The cache's mutex guards only `LoadPipeline`, `StorePipeline`, and `GetSerializedSize` with
`Serialize`; `CreatePipelineState` runs outside it, so creations compile side by side. It is a
leaf, never taken while a list records ([lock order](d3d12-backend.md#threading-and-lock-order)).

The library's outcomes arrive twice, as an HRESULT the cache reads and as a debug-layer message.
The router's deny list drops six of them (not found, duplicate name, invalid blob, driver or
adapter mismatch, unsupported); the cache reports a refused blob as its one `Warning`, and
`LOADPIPELINE_INVALIDDESC` (972) passes.

## Lazy binding

A native list has one pipeline slot for graphics and compute alike. `setPipeline` keeps the caller's
pipeline payload in the list's `Binding` and calls `SetPipelineState` only when its own side does
not already hold that state, for a compute, a graphics or a mesh pipeline. `Binding` records the
state the slot holds from each side, and any `SetPipelineState` clears the other side's record.
`setRoot` only stores the root block's address and marks both sides dirty ([root
binding](d3d12-recording.md#root-binding)); the next dispatch or draw flushes what is stale.

The compute flush, in this order, each call only where the list holds something else:

- The global root signature, when a backend kernel left its own there. The switch drops the root
  arguments, so the root becomes dirty again.
- The pipeline state.
- `SetComputeRootShaderResourceView(0, root)` when the side is dirty and a root was set; a list
  that never set one binds none.

The graphics flush is the pipeline state (with the recorded depth bias when the pipeline enables
it), then `SetGraphicsRootShaderResourceView(0, root)` on the same terms. It never switches the
graphics root signature, which no backend kernel changes. A direct draw adds `b1`,
`SetGraphicsRoot32BitConstant(1, 0, 0)`, the draw record index that `drawId` reads. `b1` is dirty
at acquisition and after every `ExecuteIndirect` of a draw signature, because each command writes
its record's index there, so the next direct draw writes 0 again and a second writes nothing. A pass
does not reset it, and an indirect draw writes no `b1` itself.

A [fill](d3d12-recording.md#fills) leaves the kernel's root signature and pipeline on the compute
side with the root dirty, so the next caller dispatch costs a root signature, a pipeline state,
the root and the dispatch (four calls) and the one after it one. A fill never touches the graphics
side. Each entry costs its native minimum plus at most one call in the steady state, each counted
once in `nativeRecordCalls`. The `[d3d12]` cases pin it: a compute `setPipeline`, a `dispatch`
after `setRoot`, the same again and a third cost 1, 2, 0 and 1; the first draw after `setRoot`
costs 3 (root, `b1`, draw) and the next 1; an indirect draw after `setRoot` costs 2, the direct
draw after it 2 and the next 1. The registered C8 sequence (barrier, pass, `setPipeline`,
viewport, scissor, draw, end) costs 14 calls in a list's first pass and 8 in each later one.

## Render passes

`beginRenderPass` runs the core's check, then in order:

1. The pass's scope, through the native half that `beginScope` uses ([scopes](
   d3d12-recording.md#scopes-and-timestamps); `D3D12Scopes.h`); `endRenderPass` ends it the same
   way, so every pass is timed like a scope, on a device created with `DeviceDesc::scopeTimings`.
2. One enhanced `Barrier` call that moves mip 0 and layer 0 of every resolve target from
   `RENDER_TARGET` to `RESOLVE_DEST` (sync `RENDER_TARGET` to `RESOLVE`). The backend owns it
   because VU-rojo-cmd-0007 puts a resolve target in `ColorAttachment` at the call, and D3D12
   resolves from a different layout ([barriers](d3d12-recording.md#barriers)).
3. `BeginRenderPass` with one render-target description per colour attachment, from the texture's
   cached RTV, and a depth-stencil description from its cached DSV. Load `Load`, `Clear` and
   `DontCare` become `PRESERVE`, `CLEAR` and `DISCARD`; store `Store` and `DontCare` become
   `PRESERVE` and `DISCARD`. A named resolve target makes the ending access `RESOLVE` with mode
   `AVERAGE`, one subresource parameter that takes the attachment's mip and layer into mip 0 and
   layer 0 of the target, and the source preserved exactly when the store op is `Store`. A format
   with no stencil plane uses `NO_ACCESS` for stencil. The flag is `ALLOW_UAV_WRITES`, since any
   stage may store through an address.
4. The starting dynamic state, filtered (next section). The pass also unbinds the caller's pipeline
   in the list's record; the native slot keeps its state, so a repeated `setPipeline` sends nothing.

`endRenderPass` calls `EndRenderPass`, then one `Barrier` that returns each resolve target to
`RENDER_TARGET`, then ends the scope. The descriptions, the resolve parameters and the barriers
live in a fixed `PassStorage` on the list, which the runtime reads until `EndRenderPass`, so a
pass allocates nothing. Each `Barrier` adds one to `nativeBarriers`, each pass one to
`encodersOpened`. A pass with no resolve costs `BeginRenderPass`, `EndRenderPass`, its scope's four
calls and at most the two starting values.

## Dynamic state

The list holds the stencil reference, the blend factor, the recorded depth-bias triple, the
viewport, the scissor, the topology and the index buffer view. All are unknown at
acquisition, since the state a native list keeps across `Reset` is not relied on. Passes leave
them as they are, and a setter sends a native call only when the value differs from the held one.

- A pass writes `OMSetStencilRef(0)` and `OMSetBlendFactor(0, 0, 0, 0)` only where the held value
  differs, so a list's first pass writes both and a second pass writes neither. It resets the
  depth-bias triple to zero without a call. It sets no viewport or scissor: the reference leaves
  them unset at a pass's start and requires them before a draw.
- A pipeline created with depth bias uses `DYNAMIC_DEPTH_BIAS`. `setPipeline` of one issues
  `RSSetDepthBias` with the recorded triple every time, unfiltered, because whether the runtime
  keeps the value across `SetPipelineState` is not established. `setDepthBias` records the triple,
  sends nothing when it equals the record, and issues it only while such a pipeline is bound.
  D3D12 takes the clamp before the slope-scaled term.
- `setPrimitiveTopology` and the topology that `setPipeline` restores for a vertex pipeline pass
  through the same filter. A mesh pipeline sets none.

## Draws and dispatches

A zero-count draw or mesh draw and a dispatch with a zero dimension record nothing. A direct draw
is the graphics flush, `b1` when dirty, then the call. `draw` is `DrawInstanced`, and
`firstInstance` is `StartInstanceLocation`, which `SV_InstanceID` excludes, so shaders cannot see
it, as chapter 05 requires. `drawMeshTasks` is `DispatchMesh` with `b1` too, because the task and
mesh entries read `drawId` as the vertex stage does (ADR 0018). `dispatch` flushes the compute side
and calls `Dispatch`; `dispatchIndirect` finds the arguments buffer in the heap directory with one
acquire load and calls `ExecuteIndirect` on the dispatch signature with a count of one.

An indexed draw sets one `D3D12_INDEX_BUFFER_VIEW` of the whole buffer from offset 0, its size the
buffer's width capped at `UINT32_MAX` and rounded down to the index size, unless the held
location, size and format are equal. `firstIndex` is `StartIndexLocation` and `baseVertex` is
`BaseVertexLocation`; direct and indirect indexed draws call the same function.

`vertexIndex` on D3D12 is plain `SV_VertexID`, which excludes `firstVertex` and `baseVertex` as it
does on every target ([ADR 0024](../decisions/0024-vertex-index-excludes-the-base.md)). The draw,
indirect and graphics ABI conformance files check it with nonzero bases on WARP (run 39660621919),
and the `[d3d12]` cases draw with base 0.

## Indirect drawing

The four command signatures (draw, indexed draw, mesh and dispatch) are made once per device, the
draw ones with the global root signature, and each draw signature writes the record's `drawId` to
`b1` (ADR 0018). An indirect draw flushes the graphics side, sets the index buffer view for the
indexed form, calls `ExecuteIndirect(signature, maxCount, arguments, arguments.hi, count, count.hi)`
and marks `b1` dirty. The arguments and count resources come from the heap directory, one acquire
load each, and the count resource is null when the entry names no count buffer.

With no count buffer exactly `maxCount` records run; with one, the `uint32` it holds caps them,
which is `ExecuteIndirect`'s own rule. A `maxCount` of 0 records nothing in Release, where nothing
checks it. Nothing is split or hoisted: `ExecuteIndirect` takes a count buffer natively and runs
inside the render pass, so a counted draw is one call. Metal 4 has no such call and splits the pass
around a compute encoder that fills a command buffer
([drawing on Metal 4](metal4-drawing.md#counted-draws)).

## Files

- `D3D12GraphicsState.cpp`: `setPipeline`, the dynamic-state setters and the graphics flush.
- `D3D12Dispatches.cpp`: the compute flush, `dispatch` and `dispatchIndirect`.
- `D3D12RenderPasses.cpp`: the passes; `D3D12Scopes.h` is the native half of a scope they share.
- `D3D12Draws.cpp`, `D3D12Indirect.cpp`: the direct and the indirect draws.
- `D3D12ListState.h`: `Binding`, `DynamicState`, `PassStorage` and the flush declarations.

## Tests

The D3D12 cases of the `abi` entry check each ABI shader library's header, entry byte sums,
`DXBC` prefixes and equality with the rule's `.dxil`, with nothing on the GPU. `[d3d12]` cases
cover the parser on hand-built blobs, the loader and its retirement, the stream and state
conversions, a pipeline for each entry of the ABI libraries, the key (hash vectors, one key for
equal descriptions, a different one for any field), the blob header rules, a second device that
hits every pipeline of the first, and concurrent creation. They run on WARP under the debug layer
and GPU-based validation.

The recording cases (`D3D12DispatchTests.cpp`, `D3D12RenderPassTests.cpp`,
`D3D12GraphicsStateTests.cpp`, `D3D12IndirectTests.cpp`) count the native calls of each entry and
assert that the debug layer and GPU-based validation raise no message. They cover the compute
flush and a fill between dispatches, passes with and without a resolve, a `DontCare` resolve store,
a resolve target resolved twice and read, stencil `NO_ACCESS`, `encodersOpened`, the filtered
starting values, pipeline and topology binds, depth bias at every `setPipeline`, the index buffer
filter, zero-count draws, `b1` after an indirect draw for vertex and mesh pipelines, `maxCount` 0,
and counts of 0 or above `maxCount` read back from the target.

Every contract and conformance file runs on WARP, the draw, indirect and graphics ABI conformance
files with `vertexIndex` excluding the base vertex, except the two upscale files under GPU-based
validation ([edges](d3d12-backend-edges.md#the-t1-run-on-warp)). Two multisample cases of the ABI
compute file expect the value `multisampleReadsSize()` of `Tests/Support/Backend.h` names: Metal 4's
exact value, the cleared colour on Null and D3D12.
