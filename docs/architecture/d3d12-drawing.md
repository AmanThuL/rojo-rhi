# Drawing on the D3D12 Backend

**Status**: Accepted

This page describes how the D3D12 backend loads shader libraries, creates pipelines and caches
them. It is implemented, verified on WARP only, not on hardware, and the backend is not a
capability ([ADR 0009](../decisions/0009-conformance-and-valid-usage.md),
[documentation](../conventions/documentation.md)). [The D3D12 backend](d3d12-backend.md)
describes the build and the device around it. Chapters 05 and 06 of the API reference stay the
authority on semantics; every check lives in `Source/Core`, so no `ROJORHI_VU` appears under
`Backends/D3D12/`. A recording entry reads native objects through `HandlePool::native` and the
list's own state, never a pool record (ADR 0020).

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
closing `dxil` line ends the text, so a reader never reads DXIL as text.

The ABI stamp lives in this container and not in the DXIL, which stays exactly what the compiler
signed and the runtime validates.

The writer checks every `--dxil NAME=PATH` file and refuses one that does not start `DXBC`,
whose size field (a little-endian `uint32` at byte 24) differs from the file's length, or whose
16-byte digest (bytes 4 to 19) is all zero, which is unsigned DXIL the runtime refuses. It names
the file. The `.dxil` of an entry is the one the shader rule already writes: `<name>.dxil` for
a shader with one entry, `<name>.<entry>.dxil` for several.

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

`d3d12::PipelineState` is the payload. It holds the pipeline state object (one reference), the
kind (graphics, mesh or compute), the list form of a vertex pipeline's topology class (undefined
for the other kinds) and whether depth bias is enabled. A bind reads these through
`pool<Pipeline>().native`, one load without a lock; recording a pipeline into a list arrives with
command recording, and `setPipeline` aborts until then. The time of the creation, a cache hit
included, adds to `pipelineCompileNanoseconds`, and the pipeline is named from its label.

A refused native creation is `d3d12::failure`: `OutOfMemory` or `DriverFailure` from the
HRESULT, naming the call and the pipeline's label, with the removal report when the device was
removed. An entry name the library lacks, which only a Release build lets past the core, is
`DriverFailure` naming the entry.

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
leaf taken only while a pipeline is created or the blob is read, never while a list records. The
lock order is in [the D3D12 backend](d3d12-backend.md#threading-and-lock-order).

The library's outcomes arrive twice, as an HRESULT the cache reads and as a debug-layer message.
The router's deny list drops six of them: a name not found, a duplicate name, an invalid
library blob, a driver version mismatch, an adapter version mismatch and an unsupported library.
The cache reports a refused blob as its one `Warning`. `LOADPIPELINE_INVALIDDESC` (972) passes.

## Tests

The D3D12 cases of the `abi` entry check, for each ABI shader library, the header, the entry
byte sums, the `DXBC` prefix of every entry and its equality with the rule's `.dxil`. Nothing runs
on the GPU. `[d3d12]` cases cover the parser on hand-built blobs (a truncated header, a count past
the end, an entry that does not start `DXBC`, a foreign magic, a mismatched stamp), the loader and
its retirement, the stream contents and state conversions, a pipeline for each entry of the ABI
libraries, the key (hash vectors, one key for equal descriptions, a different one for any field),
the blob header rules, a second device that hits every pipeline of the first, and concurrent
creation. They run on WARP under the debug layer and GPU-based validation.
