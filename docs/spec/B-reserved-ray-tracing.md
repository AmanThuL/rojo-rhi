# B. Reserved: ray tracing

**Status**: Accepted
**API version**: 0.1.0
**Entries**: none

API version 0.1.0 contains no ray-tracing interface: no acceleration-structure type, no build
command, no shader intrinsic and no capability bit. This appendix records what is reserved so a
later version can add ray tracing without changing any existing entry, enumerator value, structure
layout or valid-usage identifier (see [conventions](00-conventions.md)).

## Reserved model

Ray tracing, when added, follows the same address-first model as the rest of the interface:

- **Acceleration structures are placed in heap memory and addressed.** An acceleration structure
  is created at a `Placement` in a `Heap` (see [memory and buffers](03-memory-and-buffers.md)),
  sized from a backend query and built and updated by commands on a command list. On D3D12 and
  Vulkan it occupies buffer memory and has a GPU address; on Metal 4 it is its own resource, an
  `MTL::AccelerationStructure` created from an `MTL::Heap` at an offset and reached through its
  `gpuResourceID`. Build inputs (vertex, index, instance and scratch data) are `Addr` values.
- **Shaders trace with inline ray queries only.** A shader of any stage that can read the heaps
  issues a ray query against an acceleration structure and reads the hit result in place. This is
  the form all three APIs share.
- **Acceleration structures take heap-index space.** A shader reaches an acceleration structure
  through a 32-bit index in the resource heap, the same way it reaches a texture view through a
  `ViewIndex` (see [textures and heaps](04-textures-and-heaps.md) and
  [shader ABI](05-shader-abi.md)). The resource heap's index space is therefore not assumed to
  hold texture views alone, and nothing in 0.1.0 may encode that assumption.
- **Builds are ordered by ordinary barriers.** Build writes and ray-query reads become additional
  `Stage` and `Access` values in [synchronization](09-synchronization.md), appended without
  renumbering existing ones.

Ray-tracing pipelines with ray-generation, miss and hit shaders and a shader binding table are
not planned, because Metal 4 has none: it offers intersection function tables but no binding-table
pipeline, so such pipelines would be emulated at a `costly` class there
([Metal 4 and Slang research](../research/2026-10-11-rhi-redesign/06-metal4-slang-state.md)).
Callable shaders, intersection shaders and recursion depth fall with them.

## Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Acceleration structure | `MTL::AccelerationStructure` created from an `MTL::Heap` at an offset, built by `MTL4::ComputeCommandEncoder` | buffer in the acceleration-structure layout, built by `BuildRaytracingAccelerationStructure` | `VkAccelerationStructureKHR` over a buffer, built by `vkCmdBuildAccelerationStructuresKHR` |
| Shader reference | `gpuResourceID` in the hidden ID table, one extra 8-byte load | shader-resource view in `ResourceDescriptorHeap` | acceleration-structure descriptor in the resource heap, or its device address |
| Inline query | `intersection_query` in MSL | `RayQuery` (DXR 1.1) | `VK_KHR_ray_query` |
| Hardware | Apple6 and newer, inside the Apple7 floor | DirectX 12 Ultimate (DXR 1.1), inside the planned floor | optional extensions |
| Cost of inline queries | cheap: the structure is reached through an ID-table entry | native | native |
| Ray-tracing pipelines | costly: no binding-table pipeline, emulation only | native, not exposed | native, not exposed |

The Metal 4 floor and the planned D3D12 floor of [ADR 0010](../decisions/0010-hardware-floor.md)
already include inline ray queries, so adding them needs no new floor; on Vulkan the oracle
mapping assumes `VK_KHR_acceleration_structure` and `VK_KHR_ray_query`. Exact Metal 4
build-descriptor names are *unverified* until an entry is written against the shipping headers.
