# 05. Shader ABI (continued)

**Status**: Proposed (until spike 1 of ADR 0014 reports)
**API version**: 0.1.0

This part defines the `RojoRHI.slang` functions that dereference `Addr`, fetch textures and samplers
by heap index, mark divergent indices, and give built-ins one definition on every target. The
model, the layout rules and the root block are in [part 1](05-shader-abi.md). Every function lives
in namespace `rojoRHI`. As [part 1](05-shader-abi.md#model) explains, each entry states its
requirements as normative prose, because no tier can diagnose a shader's use of them.

The D3D12 lowerings below are designed mappings; DXIL compiled from this ABI has not yet been
validated with DXC, so each D3D12 lowering says `unverified` until the planned backend's
compile-and-run test passes.

## Divergence

An index or `Addr` base that may differ between the threads of a wave is marked by the caller with
`rojoRHI::nonUniform`. An unmarked index or base is dynamically uniform, so uniform fetches keep
their native cost on every backend.

### `rojoRHI::nonUniform`

```slang
uint nonUniform(uint index);   // a ViewIndex or SamplerIndex
Addr nonUniform(Addr base);
```

Returns its argument unchanged, marked as possibly different between the threads of a wave, for
passing to `rojoRHI::load`, `rojoRHI::store`, `rojoRHI::texture` and `rojoRHI::sampler`.

Requirements: the result is passed directly to a `rojoRHI` fetch or dereference, not stored in
memory or a shader output first.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Lowering | none: returns the argument | `NonUniformResourceIndex` on the heap index; unverified | `NonUniform` decoration on the index |
| Cost | native | native: divergent fetches may serialize in hardware, as in native code | native |

## Addresses

`Addr` dereference is native on Metal 4 and Vulkan and costly on D3D12, where HLSL and DXIL have no
pointers: an `Addr` there carries a raw shader-resource heap index and a byte offset, and each
distinct base costs a descriptor fetch. A `ShaderWrite` buffer's raw unordered-access descriptor
sits in the heap slot immediately after its raw shader-resource descriptor, so `rojoRHI::store` uses
the index plus one. Whether the fetch is cheap enough for hot paths is spike 1 of
[ADR 0014](../decisions/0014-overhead-measurement.md), still pending. Until it reports, per-draw hot
data belongs in root fields and heap indices, and an `Addr` hop is for bulk and cold data.

### `rojoRHI::load`

```slang
T load<T>(Addr base, uint index);
```

Returns the `T` at byte address `base + index * sizeof(T)`, where `sizeof(T)` and every member
offset follow natural layout, equal to the C++ `sizeof` and `offsetof` of the shared struct.
Writes by earlier GPU work are visible only when ordered by a barrier (see
[synchronization](09-synchronization.md)).

Requirements: `base` is not the null address and names a buffer that is alive when the read
executes, or frame data produced in the frame that executes it; the `sizeof(T)` bytes read lie
inside that buffer; the byte address read is a multiple of 4 and of the natural alignment of `T`;
`T` contains no texture, sampler or handle type and no pointer, buffer memory being named by `Addr`
members; and `base` is dynamically uniform across the wave or marked with `rojoRHI::nonUniform`.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Lowering | `device T*` load | `ByteAddressBuffer` from `ResourceDescriptorHeap[index]`, then `Load<T>` at offset; unverified | `PhysicalStorageBuffer` load |
| Cost | native | costly: one descriptor fetch per base; spike 1 pending | native |

### `rojoRHI::store`

```slang
void store<T>(Addr base, uint index, T value);
```

Writes `value` to byte address `base + index * sizeof(T)` under the same natural layout as
`rojoRHI::load`. The write is visible to later GPU work only when ordered by a barrier (see
[synchronization](09-synchronization.md)).

Requirements: `base` is not the null address and names a buffer created with
`BufferUsage::ShaderWrite` that is alive when the write executes; the `sizeof(T)` bytes written lie
inside that buffer; the byte address written is a multiple of 4 and of the natural alignment of
`T`; `T` contains no texture, sampler or handle type and no pointer; and `base` is dynamically
uniform across the wave or marked with `rojoRHI::nonUniform`.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Lowering | `device T*` store | `RWByteAddressBuffer` from `ResourceDescriptorHeap[index + 1]`, then `Store<T>` at offset; unverified | `PhysicalStorageBuffer` store |
| Cost | native | costly: one descriptor fetch per base; spike 1 pending | native |

## Heap indices

On Metal 4 a heap index reads an 8-byte resource ID from a hidden table the backend binds at a
reserved slot: one extra load. Spike 2 of [ADR 0014](../decisions/0014-overhead-measurement.md)
kept this documented table over the consecutive `base + i` IDs of a texture view pool, which skip
the load but rest on undocumented ID arithmetic and a handle pun MSL does not define
([spike 2](../research/2026-10-12-spike-2-texture-indexing.md)). The interface would be the same
either way.

### `rojoRHI::texture`

```slang
// T : ITexelElement, defaulting to float4; `view` is a ViewIndex.
Texture1D<T>          texture1D<T = float4>(uint view);
Texture1DArray<T>     texture1DArray<T = float4>(uint view);
Texture2D<T>          texture2D<T = float4>(uint view);
Texture2DArray<T>     texture2DArray<T = float4>(uint view);
Texture2DMS<T>        texture2DMS<T = float4>(uint view);
Texture2DMSArray<T>   texture2DMSArray<T = float4>(uint view);
Texture3D<T>          texture3D<T = float4>(uint view);
TextureCube<T>        textureCube<T = float4>(uint view);
TextureCubeArray<T>   textureCubeArray<T = float4>(uint view);
RWTexture1D<T>        rwTexture1D<T = float4>(uint view);
RWTexture1DArray<T>   rwTexture1DArray<T = float4>(uint view);
RWTexture2D<T>        rwTexture2D<T = float4>(uint view);
RWTexture2DArray<T>   rwTexture2DArray<T = float4>(uint view);
RWTexture3D<T>        rwTexture3D<T = float4>(uint view);
```

Returns the texture view stored in slot `view` of the global view heap, typed by the function. The
`texture*` forms read sampled views and the `rwTexture*` forms read storage views; a cube is
written through a two-dimensional array storage view. Textures and samplers are always separate
objects: combined texture-sampler types are not part of the ABI.

Requirements: `view` is a slot filled by `Device::createView` or `Device::writeView` that is not
destroyed or released before the access executes; the view in the slot is a sampled view for a
`texture*` form and a storage view for an `rwTexture*` form; the view's texture type, array form
and sample count match the function; `T` matches the numeric class of the view's format (see
[formats and limits](15-formats-and-limits.md)); and `view` is dynamically uniform across the wave
or marked with `rojoRHI::nonUniform`.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Lowering | 8-byte resource ID read from the hidden view table | `ResourceDescriptorHeap[view]`; unverified | resource descriptor heap |
| Cost | cheap: one extra 8-byte load, measured at about 3% for `read` and 7.5% for `sample` in a fetch-only kernel | native | native |

### `rojoRHI::sampler`

```slang
SamplerState           sampler(uint index);             // `index` is a SamplerIndex
SamplerComparisonState comparisonSampler(uint index);
```

Returns the sampler stored in slot `index` of the global sampler heap: `sampler` for filtering
samplers and `comparisonSampler` for samplers with a comparison function, used by depth
comparisons such as `SampleCmp`.

Requirements: `index` is a slot filled by `Device::createSampler` or `Device::writeSampler` that is
not destroyed or released before the access executes; the sampler in the slot has a comparison
function exactly when it is read through `comparisonSampler`, which is used only for depth
comparisons; and `index` is dynamically uniform across the wave or marked with
`rojoRHI::nonUniform`.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Lowering | sampler ID read from the hidden sampler table | `SamplerDescriptorHeap[index]`; unverified | sampler descriptor heap |
| Cost | cheap: one extra 8-byte load, measured under 0.4% against a `constexpr` sampler in a fetch-only kernel | native | native |

## Built-ins

Built-ins are wrapped because their meaning differs: D3D12's `SV_InstanceID` excludes the base
instance, and the Metal lowering of the instance index changed meaning across Slang releases. Each
wrapper has one definition on every target: `rojoRHI::vertexIndex` includes the first vertex or
base vertex, provisionally and unverified on D3D12, and `rojoRHI::instanceIndex` is zero-based. No
base value travels through a root constant, which `ExecuteIndirect` could not supply per draw.
Per-draw data in an indirect draw comes through `rojoRHI::drawId`.

### `rojoRHI::vertexIndex`

```slang
uint vertexIndex(uint vertexId);
```

Returns the index of the current vertex including the base: `firstVertex + i` for a non-indexed
draw, and the index-buffer value plus `baseVertex` for an indexed one, direct or indirect.

This definition is provisional. It rests on D3D12's `SV_VertexID` including the base vertex in
direct and `ExecuteIndirect` draws, which is unverified. The fallback, should the D3D12
compile-and-run check of the planned backend find otherwise, is one definition that excludes the
base everywhere: `vertexIndex` returns `i` or the index-buffer value alone, and a shader that needs
the base adds `firstVertex` or `baseVertex` from the per-draw record it finds through
`rojoRHI::drawId`. That check chooses between the two before this chapter is accepted.

Requirements: `vertexId` is the unmodified `SV_VertexID` input of a vertex entry point.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Lowering | `[[vertex_id]]`, which includes the base vertex | `SV_VertexID`, taken to include the base vertex; unverified | `VertexIndex` |
| Cost | native | native: unverified | native |

### `rojoRHI::instanceIndex`

```slang
uint instanceIndex(uint instanceId);
```

Returns the zero-based index of the current instance within its draw, excluding `firstInstance`,
for every draw, direct or indirect. A shader that needs the absolute instance adds a value it
carries in its own data.

Requirements: `instanceId` is the unmodified `SV_InstanceID` input of a vertex entry point.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Lowering | `[[instance_id]]` minus `[[base_instance]]`, the Slang lowering pinned by a conformance shader | `SV_InstanceID`, taken to exclude the base instance; unverified | `InstanceIndex` minus `BaseInstance` |
| Cost | cheap: one subtract | native | cheap: one subtract |

### `rojoRHI::drawId`

```slang
uint drawId();
```

Returns the draw identifier: the leading `uint32` of the current record of an indirect draw (see
[GPU-driven rendering](08-gpu-driven.md)), and 0 for a direct draw. One root serves a whole
indirect batch, so a shader uses the identifier to find its own per-draw data. A fragment shader
that needs it receives it as a flat interpolant from the stage before.

Requirements: the call is made from a vertex, task or mesh entry point.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Lowering | the backend binds the indirect record's address for the draw and the shader loads offset 0; a direct draw binds a zero identifier | root constant read from record offset 0 by the `ExecuteIndirect` command signature, or written as 0 for a direct draw; unverified | the backend binds the records' base address and stride, and the shader loads offset 0 of record `DrawIndex`; a direct draw binds a zero record, and `DrawIndex` is 0 |
| Cost | cheap: one load; unverified | native: unverified | cheap: one load |
