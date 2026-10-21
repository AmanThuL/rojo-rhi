# 05. Shader ABI (continued)

**Status**: Proposed (until spike 1 of ADR 0014 reports)
**API version**: 0.1.0

This part defines the vertex-stage input struct and the built-ins that give the draw's vertex,
instance and draw identifiers one definition on every target. The model, the layout rules and the
root block are in [part 1](05-shader-abi.md); the `Addr` accessors, heap-index fetches and
divergence markers are in [part 2](05-shader-abi-part2.md). Every function lives in namespace
`rojoRHI`. Each entry states its requirements as normative prose, because no tier can diagnose a
shader's use of them (see [part 1](05-shader-abi.md#model)).

The D3D12 lowerings below are implemented, verified on WARP only, not on hardware: the draw,
indirect, mesh and graphics ABI conformance cases ran them on WARP
([evidence](../research/2026-10-21-d3d12-warp-evidence.md)).

## Built-ins

Built-ins are wrapped because their meaning differs: D3D12's `SV_InstanceID` excludes the base
instance, and the Metal lowering of the instance index changed meaning across Slang releases. Each
wrapper has one definition on every target: `rojoRHI::vertexIndex` excludes the first vertex and
base vertex, and `rojoRHI::instanceIndex` is zero-based. No base value travels through a root
constant, which `ExecuteIndirect` could not supply per draw; a shader that needs a base adds it
from its root or its record.
Per-draw data in an indirect draw comes through `rojoRHI::drawId`.

### `rojoRHI::VertexIds`

```slang
struct VertexIds;   // fields internal to RojoRHI.slang
```

The vertex-stage system values the vertex built-ins read. A vertex entry point takes it as a
parameter and passes it to `rojoRHI::vertexIndex`, `rojoRHI::instanceIndex` and
`rojoRHI::drawId`; its fields are not part of the interface. It is a struct rather than module
globals because on Metal `[[base_instance]]` is reachable only as an entry-point parameter: a
global system-value input lands in `[[stage_in]]` when the vertex shader has varyings, and the
Metal compiler rejects that (Slang `v2026.14.1`). It carries the vertex ID, the instance ID and,
except on D3D12, the base vertex and base instance; `SV_StartVertexLocation` and
`SV_StartInstanceLocation` need Shader Model 6.8, above the 6.6 floor of
[ADR 0010](../decisions/0010-hardware-floor.md).

Requirements: the vertex entry point takes exactly one `VertexIds` parameter, unmodified.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Lowering | `[[vertex_id]]`, `[[base_vertex]]`, `[[instance_id]]` and `[[base_instance]]` entry parameters | `SV_VertexID` and `SV_InstanceID`; no base instance or base vertex | `SV_VulkanVertexID`, `SV_VulkanInstanceID`, `SV_StartVertexLocation` and `SV_StartInstanceLocation`, the raw `VertexIndex`, `InstanceIndex`, `BaseVertex` and `BaseInstance`, because Slang's plain `SV_VertexID` and `SV_InstanceID` already subtract the bases there |
| Cost | native | native: unverified | native |

### `rojoRHI::vertexIndex`

```slang
uint vertexIndex(VertexIds ids);
```

Returns the zero-based index of the current vertex: `i` for a non-indexed draw and the
index-buffer value alone for an indexed one, direct or indirect. `firstVertex` and `baseVertex` are
not included. A shader that needs the base adds `firstVertex` or `baseVertex` from its root block in
a direct draw, or from the per-draw record it finds through `rojoRHI::drawId` in an indirect one.
D3D12's `SV_VertexID` excludes both and its start-vertex system value needs Shader Model 6.8
([ADR 0024](../decisions/0024-vertex-index-excludes-the-base.md)).

Requirements: `ids` is the unmodified `VertexIds` parameter of a vertex entry point.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Lowering | `[[vertex_id]]` minus `[[base_vertex]]` | `SV_VertexID`, which excludes the base vertex; verified on WARP only ([evidence](../research/2026-10-21-d3d12-vertex-index-warp.md)) | the raw `VertexIndex` minus `BaseVertex`, subtracted once |
| Cost | cheap: one subtract | native: verified on WARP only | cheap: one subtract |

### `rojoRHI::instanceIndex`

```slang
uint instanceIndex(VertexIds ids);
```

Returns the zero-based index of the current instance within its draw, excluding `firstInstance`,
for every draw, direct or indirect. A shader that needs the absolute instance adds a value it
carries in its own data.

Requirements: `ids` is the unmodified `VertexIds` parameter of a vertex entry point.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Lowering | `[[instance_id]]` minus `[[base_instance]]` | `SV_InstanceID`, which excludes the base instance | the raw `InstanceIndex` minus `BaseInstance`, subtracted once |
| Cost | cheap: one subtract | native | cheap: one subtract |

### `rojoRHI::drawId`

```slang
uint drawId(VertexIds ids);   // vertex stage
uint drawId();                // task and mesh stages
```

Returns the draw identifier: the leading `uint32` of the current record of an indirect draw (see
[GPU-driven rendering](08-gpu-driven.md)), and 0 for a direct draw. One root serves a whole
indirect batch, so a shader uses the identifier to find its own per-draw data. A fragment shader
that needs it receives it as a flat interpolant from the stage before.

Requirements: the call is made from a vertex, task or mesh entry point, and a vertex entry point
passes its unmodified `VertexIds` parameter.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Lowering | vertex stage: `[[base_instance]]`, which the backend sets to the record's `drawId` for an indirect draw and to 0 for a direct draw; task and mesh stages: the backend binds the record's address for the draw and the shader loads offset 0, or for a direct draw binds a zero record, four bytes the list writes into its frame arena once per recording | root constant read from record offset 0 by the `ExecuteIndirect` command signature, or written as 0 by a direct draw when the list does not hold 0 there, which it does not after acquisition or an indirect draw | the backend binds the records' base address and stride, and the shader loads offset 0 of record `DrawIndex`; a direct draw binds a zero record, and `DrawIndex` is 0 |
| Cost | native in the vertex stage; cheap in task and mesh stages: one load | native: unverified | cheap: one load |
