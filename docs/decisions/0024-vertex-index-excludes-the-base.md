# ADR 0024: vertexIndex excludes the base vertex on every target

**Status**: Accepted (2026-10-20)

## Context

[ADR 0011](0011-shader-abi.md) made `rojoRHI::vertexIndex` include the first vertex or base
vertex. It rested on D3D12's `SV_VertexID` doing so, which was unverified, and it named the
base-excluded definition as the fallback a test may select. [ADR 0018](
0018-root-srv-and-reserved-bindings.md) lowered the SPIR-V target to the raw `VertexIndex`.

D3D12's `SV_VertexID` excludes `StartVertexLocation` and `BaseVertexLocation`; verified on WARP
only, in direct draws ([evidence](../research/2026-10-21-d3d12-vertex-index-warp.md)). Shader
Model 6.8 adds `SV_StartVertexLocation` for exactly that value, above the 6.6 floor of
[ADR 0010](0010-hardware-floor.md). Vulkan layers that emulate D3D semantics subtract `BaseVertex`
for the same reason.

## Decision

- The fallback is the definition. `vertexIndex` returns `i` for a non-indexed draw and the
  index-buffer value alone for an indexed one, direct or indirect: Metal `[[vertex_id]]` minus
  `[[base_vertex]]`, Vulkan the raw `VertexIndex` minus `BaseVertex`, D3D12 `SV_VertexID`.
- A shader that needs the base adds `firstVertex` or `baseVertex` from its root in a direct draw,
  or from the per-draw record it finds through `drawId` in an indirect one.
- This amends the sentences of ADR 0011 that `vertexIndex` includes the base natively and that
  D3D12's `SV_VertexID` is taken to include it, and the sentences of ADR 0018 that SPIR-V uses
  the raw `VertexIndex` and that `VertexIds` holds only the vertex ID, the instance ID and the
  base instance (it now holds the base vertex too, except on D3D12); nothing else in either
  changes.
- `ROJORHI_SHADER_ABI_VERSION` becomes 2. The value a built-in returns changes, and a library
  compiled against 1 would load and read base-included indices.

## Consequences

- `vertexIndex` and `instanceIndex` have the same shape: zero-based, one subtract on Metal and
  Vulkan, native on D3D12.
- `firstVertex` and `baseVertex` are invisible to shaders. They keep their place in the draw calls
  and records for parity with the native calls.
- Chapter 05's ABI contract includes the value each built-in returns, so the rule of
  [ADR 0023](0023-d3d12-store-marks-its-index.md), no bump when bytes and values are unchanged,
  still holds.
