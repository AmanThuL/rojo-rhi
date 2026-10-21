# What D3D12's `SV_VertexID` holds in a draw with a base, on WARP

**Status**: Accepted (2026-10-21, WARP only, not hardware)

[ADR 0024](../decisions/0024-vertex-index-excludes-the-base.md) rests on this note. It records the
compile-and-run check [ADR 0011](../decisions/0011-shader-abi.md) left open: whether D3D12's
`SV_VertexID` includes `StartVertexLocation` and `BaseVertexLocation`.

## Run

CI run [39529480563](https://github.com/AmanThuL/rojo-rhi/actions/runs/39529480563), job `d3d12`,
2026-10-21: WARP 1.0.21, Agility SDK 1.619.6, DXC 1.9.2607 through Slang, Debug under the debug
layer and GPU-based validation, and Release. `Tests/Conformance/DrawConformanceTests.cpp` ran
with its earlier base-included expectations against the module's D3D12 lowering, `SV_VertexID`,
which this change keeps. WARP is the software adapter; nothing here is hardware evidence.

## Findings

- 11 of the file's 13 cases pass. The 2 that fail are exactly the cases with a base, in Debug and
  Release alike.
- `draw` with `firstVertex` 3 writes 1.0 where a base-included index gives 4.0, and with 999 it
  writes 1.0 where 1000.0 is expected: the shader sees the vertex number without
  `StartVertexLocation`.
- `drawIndexed` with `baseVertex` 27 writes 4.0 where 31.0 is expected, and with -3 it writes 4.0
  where 1.0 is expected, for 16-bit and 32-bit indices: the shader sees the index-buffer value
  without `BaseVertexLocation`.

## Conclusion

`SV_VertexID` excludes the base vertex in direct draws on WARP. Indirect draws were not run here;
`ExecuteIndirect` takes the same draw arguments, and the indirect conformance cases cover it once
they run on D3D12. Shader Model 6.8's `SV_StartVertexLocation` exists to recover the excluded
value, and it is above the 6.6 floor of [ADR 0010](../decisions/0010-hardware-floor.md).
