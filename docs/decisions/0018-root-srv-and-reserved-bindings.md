# ADR 0018: The root block reaches D3D12 as a root SRV, and the module owns the bindings

**Status**: Accepted (2026-10-13)

## Context

[ADR 0011](0011-shader-abi.md) fixed one root block per draw or dispatch and named its D3D12 form
as a root constant buffer view at `b0`. Writing `Shaders/RojoRHI/RojoRHI.slang` showed that form
cannot be generic. Probes against Slang `v2026.14.1` found:

- `root<T>()` is generic over the root type, and `ByteAddressBuffer.Load<T>(0)` compiles for Metal,
  SPIR-V and HLSL. On Metal it binds `[[buffer(0)]]`, as chapter 05 requires.
- A constant buffer needs `ConstantBuffer<T>` with `T` at global scope. A generic function cannot
  declare it, so no generic form reaches a root CBV.
- The generic form on HLSL is a raw `ByteAddressBuffer` at `t0`, which D3D12 binds as a root SRV by
  GPU virtual address.
- On Metal, `[[base_instance]]` is reachable only as an entry-point parameter. A global
  system-value input lands in `[[stage_in]]` when the vertex shader has varyings, and Metal
  rejects it.
- On SPIR-V, Slang's plain `SV_VertexID` and `SV_InstanceID` already subtract the base values, so
  the raw Vulkan indices need `SV_VulkanVertexID` and `SV_VulkanInstanceID`.

## Decision

**Root lowering.** `root<T>()` is one function with a body per target:

| | Lowering |
|---|---|
| Metal 4 | a raw buffer at argument-table `buffer(0)`, read as `T` at offset 0 |
| SPIR-V | `T` loaded through the root `Addr` held in push data, as a physical storage buffer pointer |
| HLSL and DXIL | a raw `ByteAddressBuffer` at `t0`, bound on D3D12 as a root SRV by GPU virtual address |

This amends [ADR 0011](0011-shader-abi.md) in four places, and nothing else in it changes:

- The D3D12 cell of the root table: root SRV `t0`, not root CBV `b0`.
- The root-CBV consequence, "one root-CBV write on D3D12": the write is a root-SRV write.
- The shader-build sentence, "to DXIL once the planned D3D12 backend exists, and to SPIR-V in CI
  only": the rule emits MSL, SPIR-V and HLSL on every host at every build, a metallib when the Metal
  toolchain exists and DXIL when a DXC loads.
- The built-in signatures of `vertexIndex()`, `instanceIndex()` and `drawId()` in the vertex stage,
  which take `VertexIds`.

The root is read in natural layout on every target. The flat root rules of chapter 05 make
natural layout equal to constant-buffer layout, and the build-time layout check enforces the rules,
so a root type means the same bytes everywhere. `VU-rojo-abi-0002`, the 256-byte alignment of the
root address, stays: frame data is already placed that way and a looser rule can follow later
without breaking callers.

**Reserved bindings.** The module owns every binding a shader has; a consumer shader declares
none. Changing any row bumps `ROJORHI_SHADER_ABI_VERSION`.

| Use | Metal 4 argument table | D3D12 | Vulkan |
|---|---|---|---|
| Root | `buffer(0)` | root SRV `t0` | push block: root `Addr` |
| View ID table | `buffer(1)` | `ResourceDescriptorHeap` | resource heap, `__slang_resource_heap` |
| Sampler ID table | `buffer(2)` | `SamplerDescriptorHeap` | sampler heap, `__slang_resource_heap` |
| Draw record | `buffer(3)`, mesh and task stages only; the vertex stage reads `[[base_instance]]` | root constant `b1` | push block: record `Addr` and stride |

On D3D12 and Vulkan every stage that reads the record, vertex included, reads it from the root
constant or push block. The Vulkan push block is `{root Addr, record Addr, recordStride}`: `root` at
offset 0, `record` at offset 8 and `recordStride` (a `uint`) at offset 16, 24 bytes in std430. The
Vulkan heaps are the runtime descriptor arrays Slang names `__slang_resource_heap`: samplers at set
1 binding 0 and sampled and storage images at set 1 binding 2.

**Target selection.** The shader rule passes exactly one of `-D ROJORHI_TARGET_METAL`,
`-D ROJORHI_TARGET_SPIRV` and `-D ROJORHI_TARGET_HLSL` per compile, HLSL also for DXIL. The module
fails with `#error` when none or more than one is defined, so compiling it outside the rule fails
loudly instead of producing a library with the wrong bindings.

**Vertex built-ins take a parameter.** A new entry, `rojoRHI::VertexIds`, is a struct a vertex
entry point takes as a parameter. Its fields are internal: the vertex ID, the instance ID and the
base instance, except on D3D12, where `SV_StartInstanceLocation` needs Shader Model 6.8, above the
6.6 floor of [ADR 0010](0010-hardware-floor.md). The built-ins become `uint vertexIndex(VertexIds)`,
`uint instanceIndex(VertexIds)` and `uint drawId(VertexIds)` in the vertex stage, and `uint
drawId()` in the task and mesh stages. SPIR-V uses Vulkan semantics: the raw `VertexIndex`, and
`InstanceIndex` minus `BaseInstance` once. Chapter 05 is Proposed and API 0.1.0 is unreleased, so
this changes the reference's signatures without a version bump.

**Mapping corrections.** `nonUniform(Addr)` needs no decoration on SPIR-V, because an `Addr` is a
pointer rather than a descriptor index. The `uint` marker cannot reach a heap handle, so on SPIR-V
every texture, storage-texture and sampler fetch is marked `NonUniform`, which is correct for a
uniform index too; those rows are `cheap`. Metal heap fetches go through the ID tables with a Slang
`__intrinsic_asm`. `texture2DMS` and `texture2DMSArray` compile everywhere, but the pinned Slang
emits invalid MSL for their `Load` and `GetDimensions` on Metal, so they cannot be read there; that
entry stays unverified until compile-and-run. The D3D12 `Addr` is the raw-buffer heap index in `lo`
and a 32-bit byte offset in `hi`; a load uses `index` and a store uses `index + 1`. The emitted
HLSL has one `NonUniformResourceIndex`, in the return of the helper behind `nonUniform`; every heap
indexing site (`load`, `store`, texture, sampler) is a plain heap index. Whether DXC keeps the
qualifier through that function return, the `uint2` pack and unpack and the `+ 1` is verified with
the D3D12 backend, whose gate inspects the DXIL handle flags for each fetch kind.

**Natural layout on SPIR-V.** `load<T>` dereferences natural layout, so the shader rule compiles
SPIR-V with `-fvk-use-scalar-layout`, and the layout probe reflects it, so the check compares the
layout the shader reads. With the flag a `Vertex { float3 pos; float2 uv; }` behind an `Addr` passes
`spirv-val --scalar-block-layout`, and fails without it. The Vulkan mapping of natural layout behind
an `Addr` therefore presumes the `scalarBlockLayout` feature. One SPIR-V profile, Slang's default,
serves the probe and the conformance compiles. Slang's natural layout aligns a `uint2` to 4 bytes
and C++'s `alignas(8)` `Addr` to 8, so an `Addr` in a record sits at an 8-byte offset with explicit
padding.

## Consequences

- The D3D12 backend binds the root with `Set*RootShaderResourceView` on `t0`; the root signature
  has an SRV in that slot and a root constant at `b1`, and no root CBV.
- A raw SRV has no uniform-range limit, so the 65536-byte root cap is the portable cap kept from
  the CBV rather than a D3D12 limit.
- The shader rule prints a notice when the metallib or DXIL output is skipped and a line when it is
  enabled, so a CI log shows which optional outputs were built.
- Every D3D12 mapping in chapter 05 stays `unverified` until the planned backend's compile-and-run
  test passes, and the chapter's status stays Proposed until spike 1 of
  [ADR 0014](0014-overhead-measurement.md) reports.
- A vertex entry point changes its signature to take `rojoRHI::VertexIds`; no shader exists yet
  that this breaks.
- Moving the Slang pin re-checks the lowerings above, because several rest on what that release
  emits.
