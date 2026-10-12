# ADR 0011: One shader ABI for every backend

**Status**: Accepted (2026-10-12)

## Context

The v1 interface binds data through indexed buffer, texture and sampler slots. That model needs
per-pipeline binding declarations and a binding call per resource, and it does not map onto the
bindless, address-first model the v2 interface adopts ([0007](0007-address-first-interface.md)).
The shader side of that model has to compile for Metal Shading Language, HLSL and SPIR-V and run on
Metal 4 now, with D3D12 as the planned second backend. The
[Metal 4 and Slang note](../research/2026-10-11-rhi-redesign/06-metal4-slang-state.md) and the
[D3D12 and Vulkan note](../research/2026-10-11-rhi-redesign/05-d3d12-vulkan-state.md) established
what each target can express:

- Slang lowers pointers natively to Metal and SPIR-V. For HLSL it emits `Ptr<...>` text that is not
  HLSL and reports no error, and DXIL has no pointers.
- Metal 4 has no inline root bytes, so every root block is memory bound by address.
- A texture heap index is a 32-bit value on D3D12 and Vulkan; on Metal it is an 8-byte resource
  identifier.
- Nested structs in a constant buffer start on 16-byte boundaries on HLSL and std140 SPIR-V but not
  on Metal, and a three-component vector takes 16 bytes on Metal and 12 elsewhere.
- Slang's Metal bindless support is young and fails silently in places.

## Decision

The slot model (`bindBuffer`, `bindTexture`, `bindSampler`, `bindStorageBuffer`,
`bindStorageTexture`) is removed. A shader reaches all data through three paths: one root block,
addresses inside it, and heap indices inside it. There are no descriptor sets, no pipeline layouts
and no per-pipeline binding declarations; every backend uses one fixed global layout.

**Root block.** Every draw and dispatch reads one root block, bound by address with
`CommandList::setRoot(Addr)`, or allocated, copied and bound in one call with
`CommandList::pushRoot(const T&)`. Static roots are built once and never re-uploaded.

| | Metal 4 | D3D12 (planned) | Vulkan (oracle) |
|---|---|---|---|
| Root | argument table `buffer(0)` set by address | root CBV `b0` by GPU virtual address in one global root signature | push data holding the address, or a push-constant `Addr` |
| Cost | native | native | native |

The root block is flat: no nested structs and no three-component vectors. It is 16-byte aligned and
placed at 256-byte alignment, which D3D12 requires of a root address.

**Addresses.** `Addr` is an 8-byte value naming buffer memory. In shaders it is a `uint2` type
alias (`typealias Addr = uint2;` in Slang), never a struct. In C++ it is
`struct alignas(8) Addr { uint32_t lo, hi; }`. Every `Addr` field in a root block sits at an
8-byte-aligned offset. A struct member in an HLSL `cbuffer` starts on a 16-byte boundary, which
would lay the flat root out differently on Metal and D3D12; an 8-aligned `uint2` never straddles a
16-byte row, so every target agrees on the offsets. The layout check below is the enforcement.

| | Metal 4 | D3D12 (planned) | Vulkan (oracle) |
|---|---|---|---|
| Representation | GPU virtual address | raw-buffer heap index and byte offset | buffer device address |
| Dereference | `device T*` load | `ByteAddressBuffer` from `ResourceDescriptorHeap[index]`, then `Load<T>` | `PhysicalStorageBuffer` load |
| Cost | native | costly: one descriptor fetch per base | native |

Because the D3D12 form is not a virtual address, shared structs carry `Addr`, never a raw 64-bit
pointer, and only `RojoRHI.slang` dereferences it. Until the first spike in
[0014](0014-overhead-measurement.md) reports, the reference recommends root fields and heap indices
for per-draw hot data and confines `Addr` chasing to data that is not.

**Heap indices.** `ViewIndex` and `SamplerIndex` are `uint32` fields in roots and in any shader
data. On Metal 4 the backend binds a hidden table of 8-byte IDs at a reserved slot, costing one
extra load (`cheap`); on D3D12 and Vulkan the index addresses the descriptor heap directly
(`native`). Metal 4 also hands out consecutive resource IDs from a texture view pool, so `base + i`
addresses the right texture on current hardware, but that arithmetic is undocumented. The second
spike decides between it and the documented table; the table is the default until it reports.

**The Slang module.** The library ships `Shaders/RojoRHI/RojoRHI.slang`, the module `RojoRHI` whose
symbols live in namespace `rojoRHI` ([ADR 0001](0001-identity-and-naming.md)), and the only place
that knows how each target lowers the ABI. Every consumer shader imports it. It provides
`root<T>()`, `load<T>` and `store<T>` on `Addr`, texture and sampler fetch by index for every
texture type, and `vertexIndex()`, `instanceIndex()` and `drawId()` with one definition on every
target. Built-ins are wrapped because they differ: D3D12's `SV_InstanceID` excludes the base
instance, and the meaning of the Metal lowering changed across Slang releases. `vertexIndex`
includes the base vertex natively, `instanceIndex` is zero-based, and per-draw data reaches a shader
through `drawId`; no base value travels through a D3D12 root constant. That D3D12's `SV_VertexID`
includes the base vertex and `SV_InstanceID` excludes the base instance is unverified until the
planned backend's compile-and-run test passes, as the reference marks it; the reference also records
the fallback, a base-excluded `vertexIndex` everywhere, that the test may select.

**Vertex input.** Vertices are always pulled by the shader; there is no vertex-input state. Index
buffers stay a per-draw argument, because hardware index fetch and the post-transform cache are
worth keeping.

**Layout checking.** Every struct shared between C++ and Slang is checked at build time: a tool
compares Slang reflection offsets on each target with the C++ `offsetof` values and fails the
build on drift. Data behind an `Addr` uses natural layout on every target.

**Build and ABI version.** The xmake shader rule compiles Slang offline to a metallib for Metal 4,
to DXIL once the planned D3D12 backend exists, and to SPIR-V in CI only, so the Vulkan oracle is
checked mechanically rather than on paper. `RojoRHI.slang` declares `ROJORHI_SHADER_ABI_VERSION`;
the rule embeds it in each compiled library, and `Device::loadShaderLibrary` returns an error for a
mismatch instead of running a stale shader. A compile-and-run test of the exact ABI on every target
runs on every Slang upgrade, because the failures above are silent. RojoRHI loads precompiled
libraries and never compiles shaders at run time.

## Consequences

- Binding collapses to a root address per draw, so the CPU cost of binding is one table write on
  Metal 4 and one root-CBV write on D3D12, with no per-resource calls.
- D3D12 pays a descriptor fetch for each `Addr` base, and divergent bases need
  `NonUniformResourceIndex`. This is the reason `Addr` chasing is not recommended for hot paths
  before the first spike reports; if the spike shows it cheap, the recommendation widens without
  an interface change.
- Every allocation reachable through an `Addr` needs a raw-buffer descriptor on D3D12, and every
  pointer target must be resident on Metal 4. Both costs belong to the backend.
- The shader ABI version is separate from the library version ([0015](0015-versioning.md)): a
  library built from one release can reject shaders compiled against another.
- Moving the Slang pin re-runs the compile-and-run test and re-checks the built-in wrappers before
  the new pin is accepted.
