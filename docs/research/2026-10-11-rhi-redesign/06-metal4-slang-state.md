# Metal 4 and Slang as of October 2026: state, No Graphics API mapping and one shader ABI

**Status**: Proposed

Research input for the top-down RHI redesign. RojoRHI ships Metal 4 only; D3D12 is a design target
(ADR 0004) and Vulkan a later candidate. This note records what Metal 4 and Slang offer today, how
the current backend uses them, and how one Slang shader ABI in the "No Graphics API" style (GPU
pointers, an indexable texture heap, one root pointer, stage barriers, minimal PSO) lowers to
Metal 4, D3D12 and Vulkan [n1]. Tags: **[V]** verified here by compiling or running code; **[D]**
stated in an official source; **[U]** unverified. Experiments ran on an Apple M3 Max (Apple9) under
macOS 27.0.1 with Xcode 26.6 (MacOSX 26.5 SDK, `metal` 32023.883), Metal API and GPU shader
validation on, against `slangc` 2026.19 (latest) and 2026.14.1 (the pin in `xmake/setup.lua`).

## 1. Findings that change the design space

1. **WWDC26 left the Metal 4 core untouched.** The metal-cpp macOS 27 drop differs from the pinned
   26.4 headers only in MSL 4.1 (`LanguageVersion4_1`), tensor quantisation and copies, MetalFX
   frame interpolation and sub-rectangle/distortion inputs, and `CAMetalLayer` EDR [V][a1]. MSL
   4.1 adds acquire/release ordering for barriers and atomics, placement new and clamp-to-edge and
   offset texture reads [D][a2]. Argument tables, residency sets and barriers are as in 2025.
2. **An app-indexed texture heap works on Metal 4.** `MTLTextureViewPool` hands out contiguous
   `MTLResourceID`s (`baseResourceID + i`) [D][a3]; a kernel that reinterprets `base + i` as a
   `texture2d` read the right textures under GPU validation [V]. The reinterpretation itself is
   undocumented, so the portable path is a table of 8-byte IDs in a buffer, also verified [V].
3. **`DescriptorHandle<T>` is 8 bytes on every target**: `uint2` heap indices on HLSL and SPIR-V,
   the native `MTLResourceID` on Metal [D][s1]. Building a handle from an index is a compile error
   on Metal [D][s1], so a 32-bit texture index needs an explicit per-target fetch (section 5).
4. **Slang pointers do not reach D3D12.** For `-target hlsl` both 2026.14.1 and 2026.19 emit
   `Ptr<T, ...>` text that is not HLSL, with no diagnostic [V]. Metal and SPIR-V lower `T*`
   natively [V]. A portable ABI needs an `Addr` type with a target switch (section 5).
5. **Metal 4 has no `setBytes`** [V][a4]: every root or push-constant block is memory bound by
   address, which is exactly RojoRHI's `bindFrameData` path (ADR 0006).
6. **Metal 4 has an intra-pass barrier** (`barrierAfterEncoderStages:`) [D][a4]; the "no in-pass
   barrier" rule in `CommandList::dispatch` is RojoRHI's restriction, not Metal's.
7. **The Slang pin is behind.** 352 commits separate 2026.14.1 from 2026.19, including Metal mesh
   fixes and a semantic change: `SV_InstanceID` on Metal now excludes the base instance [D][s6].
   slang-rhi's Metal backend still uses the Metal 3 API, so it is no Metal 4 reference [V][s5].

## 2. Metal 4

### 2.1 Platform and hardware floor

Metal 4 needs macOS 26 (Tahoe) or later on Apple silicon; on the GPU side its programming model
starts at Apple7 (M1, A14) [D][a5][a6]. Selected limits and families from the May 2026 feature set
tables [D][a6]:

| Item | Value |
|---|---|
| Argument tables, allocators, command barriers, flexible pipelines, view pools, tensors | Apple7 (residency sets Apple6) |
| Placement sparse buffers and textures | Apple8; some Apple7 including every Apple7 Mac (`supportsPlacementSparse`) |
| Argument buffers tier 2 | Apple6 (M3 Max reports tier 2 [V]) |
| Argument-table slots per stage: buffers / textures / samplers | 31 / 128 / 16 |
| Reachable through argument buffers per stage: textures / samplers | 1M / 996 (Apple7-8), 500K (Apple9+) |
| Minimum constant buffer offset alignment | 4 B |
| Mesh shading; ray tracing; 64-bit atomics, indirect mesh draws, MetalFX denoiser | Apple7; Apple6; Apple9 |

### 2.2 Commands and residency

`MTL4CommandQueue` commits arrays of reusable, non-retaining `MTL4CommandBuffer`s from any thread;
memory comes from one `MTL4CommandAllocator` per frame in flight. The compute encoder merges
compute, blit and acceleration-structure work; render passes can suspend and resume across command
buffers [D][a3]. Every resource is untracked (`hazardTrackingMode` has no effect) [D][a7], and
everything a shader can reach must sit in a committed `MTLResidencySet` on the queue or command
buffer; Apple advises few large sets, queue-attached when stable [D][a8].

### 2.3 Binding

- `MTL4ArgumentTable`: `setAddress:` takes a raw `MTLGPUAddress`; `setTexture:`,
  `setSamplerState:` and `setResource:` take an `MTLResourceID`. One table serves many stages and
  encoders and is snapshotted per draw or dispatch [D][a8].
- Buffers expose `gpuAddress`; textures, samplers (`supportArgumentBuffers = YES`) and acceleration
  structures expose a 64-bit `gpuResourceID`. Tier-2 argument buffers are plain memory the CPU or
  GPU writes those values into [D][a2]. Observed IDs are small integers (textures 1-3, a sampler 2,
  a pool base 4), so they look like indices into driver-owned texture and sampler tables, with
  separate namespaces [V]; Apple does not document this and it must not be relied on.
- View pools pre-allocate slots; `setTextureView:atIndex:` returns the slot's ID without
  allocating, even while encoding [D][a8]. RojoRHI caches heavyweight `newTextureView`s instead.
- MSL accepts a texture-handle pointer only as a cast from an integer address
  (`(device texture2d<float>*)addr`); as a `[[buffer]]` parameter or a struct member it is rejected
  [V]. That one rule explains Slang issue #11970 and section 4.3, and narrows 01 section 2: the
  wrapper struct is needed for parameters and members, not for a cast-and-index.

### 2.4 Barriers

Three barriers, each `(after stages, before stages, visibility)` over `MTLStages`
(Vertex, Fragment, Tile, Object, Mesh, Dispatch, Blit, AccelerationStructure, MachineLearning,
ResourceState, All) with visibility None (execution only), Device or ResourceAlias [V][a4]:
consumer `barrierAfterQueueStages:beforeStages:` (orders earlier encoders), producer
`barrierAfterStages:beforeQueueStages:` (orders later encoders) and intra-pass
`barrierAfterEncoderStages:beforeEncoderStages:`, plus `MTLFence` update/wait and events across
queues. None names a resource. Whether an intra-pass render barrier may name Fragment on its after
side is [U]: the feature table forbids it for Metal 3 `memoryBarrier` on Apple3-10 [D][a6].

### 2.5 Pipelines and compilation

- `MTL4Compiler` compiles sync or async with QoS inheritance. Flexible render pipelines compile
  once with `MTLPixelFormatUnspecialized`, `MTLColorWriteMaskUnspecialized` and
  `MTL4BlendStateUnspecialized`, then `newRenderPipelineStateBySpecializationWithDescriptor:` per
  attachment set, at a usually small, occasionally large, fragment cost [D][a8].
- Depth-stencil is a separate object and cull, depth bias, viewport and scissor are encoder
  state, so the PSO is already close to the No Graphics API one. Offline, a
  `MTL4PipelineDataSetSerializer` harvest feeds `metal-tt` to build an `MTL4Archive`; misses fall
  back to the compiler [D][a8]. Specialisation constants are scalar function constants only.

### 2.6 Memory

Apple silicon is UMA: `Shared` buffers are CPU- and GPU-visible with `contents` and `gpuAddress`,
and `Private` keeps GPU-only data such as render targets. Placement heaps (`MTLHeapTypePlacement`)
give CUDA-style sub-allocation with `heapBufferSizeAndAlign`/`heapTextureSizeAndAlign`; aliasing is
ordered with `ResourceAlias` visibility. Placement sparse resources map pages from a placement heap
through `MTL4CommandQueue updateBufferMappings`/`updateTextureMappings`, ordered with other queue
work [D][a8].

### 2.7 GPU-driven work, geometry, ray tracing, ML and MetalFX

- Indirect draws and dispatches read arguments by GPU address, one draw per call. There is no
  draw-count multi-draw; GPU-sized batches go through indirect command buffers, executed with a
  GPU-written `MTLIndirectCommandBufferExecutionRange` [D][a4]. ICB commands bind buffers per
  command, which is how a GPU-written per-draw root address works.
- Mesh shaders replace tessellation [D][a3]. Ray tracing has intersection function buffers but
  no DXR-style binding table, so the portable subset is inline ray queries. Tensors, the ML
  encoder and in-shader tensor ops target M5/A19 Neural Accelerators [D][a9].

## 3. What the current backend does

| Area | Current Metal 4 backend |
|---|---|
| Submission | One `MTL4CommandQueue`; one reused `MTL4CommandBuffer`; three allocators and three argument tables rotated by a shared-event frame counter |
| Binding | Argument table of 16 buffer / 16 texture / 8 sampler slots, `initializeBindings`; buffers by `gpuAddress`, textures and samplers by `gpuResourceID` |
| Per-frame data | `bindFrameData`: 256 KiB pages per frame slot, `Shared`, bump allocation plus one `setAddress` (ADR 0006) |
| Residency | One device residency set, committed on resource create and destroy; the layer's set added to the queue |
| Barriers | Accumulated into a pending stage mask; emitted as one consumer queue barrier on the next encoder; ranges are declarative only |
| Pipelines | `MTL4Compiler`, fully specialised render and compute pipelines; separate depth-stencil object; raster state replayed at bind |
| Memory | Buffers `Shared`; textures `Private` unless CPU-visible; placement heaps for aliasing; views cached per texture |
| Shaders, extras | Slang to MSL to `.metallib` at `-std=metal4.0`, slot-bound globals; counter-heap timestamps; MetalFX temporal scaler |

Missing for the target model: an indexable texture heap, pointers in shared structs, intra-pass
barriers, unspecialised pipelines, view pools and ICBs.

## 4. Slang

### 4.1 Version and targets

Latest is v2026.19 (2026-09-29); RojoRHI pins v2026.14.1 [V][s2]. Targets: `metal` (MSL),
`metallib`, `hlsl`/`dxil` (DXIL needs DXC, absent here) and `spirv`. Metal is still "work in
progress" [D][s3]: inline ray tracing landed (#9926), an RT-pipeline design is open (#11296).

### 4.2 Pointers

| Target | `T*` / `Ptr<T>` | Evidence |
|---|---|---|
| SPIR-V | `PhysicalStorageBuffer64` pointers | [V]; documented [s1] |
| Metal | `device T*`, natural (packed) layout | [V] compiles and runs; the guide still lists only SPIR-V, C++, CUDA [s1] |
| HLSL / DXIL | Invalid `Ptr<...>` text, no diagnostic | [V] 2026.14.1 and 2026.19; DXIL has no pointers through SM 6.10 [D][d1] |

On Metal an entry-point `uniform Root*` is wrapped in a struct at `[[buffer(0)]]`, an extra
indirection (#10675); `ConstantBuffer<Root>` binds the root block itself there [V].

### 4.3 Bindless and blocks

- `DescriptorHandle<T>` (`T.Handle`): HLSL `ResourceDescriptorHeap[h.x]` / `SamplerDescriptorHeap`;
  SPIR-V either a global descriptor array in a reserved set (`-bindless-space-index`) or, with
  `-capability spvDescriptorHeapEXT`, `ResourceHeapEXT`/`SamplerHeapEXT` built-ins and no
  descriptor sets [V]; Metal the resource itself [D][s1]. Combined texture-samplers are not
  supported through handles on Metal [D][s4]. A custom `getDescriptorFromHandle` can override the
  fetch [D][s1].
- Natural code, `struct Root { Texture2D.Handle* textures; }`, yields MSL that Apple's compiler
  rejects (a texture pointer as a struct member) [V]; unsized resource arrays fail too (#11970). An
  address carried as `uint2` and cast at the point of use compiles and runs [V].
- `ParameterBlock<T>` becomes a Metal argument buffer, D3D12 table or Vulkan set [D][s3]; Metal
  reflection reports its members in slots, not tier-2 bytes (#12544), a poor ABI for this design.
- `[[vk::push_constant]] ConstantBuffer<T>` lowers to `constant T* [[buffer(0)]]` on Metal,
  `cbuffer : register(b0)` on HLSL and a push-constant block on SPIR-V; JSON reflection reports
  `constantBuffer 0`, `constantBuffer 0` and `pushConstantBuffer 0` [V].

### 4.4 Reflection, speed and tooling

Reflection (API or `-reflection-json`) gives per-target bindings and offsets, and
`IBindlessResourceMetadata::usesBindlessResourceHeap()` says whether a heap must be bound [D][s1].
One small kernel takes about 80 ms to MSL, 100 ms MSL to AIR, 290 ms via `-target metallib` [V].
Precompiled `.slang-module`s amortise front-end work, and `-depfile` exists in both versions [V].
The `metallib_4_0` capability emits `required_threads_per_threadgroup` (needs `-std=metal4.0`).

## 5. One shader ABI for three backends

### 5.1 The ABI

GPU data is reached through an 8-byte `Addr`, textures and samplers through 32-bit indices, and
the root block is bound by address at slot 0. The full listing compiles for all three targets with
2026.19, and its MSL samples the right textures through ID tables on Metal 4 under validation [V].

```slang
struct Addr { uint2 bits; };   // Metal/SPIR-V: GPU address. D3D12: (raw-UAV heap index, byte offset).
T loadAt<T>(Addr a, uint i) {
    __target_switch {
    case hlsl: RWByteAddressBuffer b = ResourceDescriptorHeap[a.bits.x];
               return b.Load<T>(a.bits.y + i * sizeof(T));
    default:   T* p = (T*)bit_cast<uint64_t>(a.bits); return p[i];
    }
}                                                    // storeAt<T> is symmetric.
Texture2D<float4> fetchTexture(Addr metalTable, uint index) {
    __target_switch {
    case metal: Texture2D<float4>.Handle* t = (Texture2D<float4>.Handle*)bit_cast<uint64_t>(metalTable.bits);
                return t[index];                    // 8-byte MTLResourceID table (or view-pool IDs)
    default:    Texture2D<float4> t = ResourceDescriptorHeap[index]; return t;
    }
}                                                    // fetchSampler: same with SamplerDescriptorHeap.
struct Root { Addr textureTable; Addr samplerTable; Addr materials; Addr output; uint count; };
[[vk::push_constant]] ConstantBuffer<Root> gRoot;    // Metal buffer(0), HLSL b0, SPIR-V push constants
```

| Element | Metal 4 | D3D12 (design target) | Vulkan |
|---|---|---|---|
| Root block | `constant Root* [[buffer(0)]]`: `setAddress(arena + offset, 0)` | `cbuffer b0`: root CBV by GPU VA (256-byte alignment) or root constants | push-constant block, or 8 bytes of push data holding its address |
| `Addr` deref | `device T*` from the integer, one load | raw view from the heap, then `Load` at offset | `PhysicalStorageBuffer` load |
| Texture index | read 8-byte ID from a table (or view-pool base + index), one extra load | `ResourceDescriptorHeap[i]` | descriptor heap or descriptor-indexing array |
| Sampler index | read sampler ID from a table (996 / 500K reachable) | `SamplerDescriptorHeap[i]` (2,048 limit) | sampler heap |
| Heap binding | none; tables are memory, entries in a residency set | heap-indexed root-signature flags | `ResourceHeapEXT`, no sets |

### 5.2 Layout rules the shared C++ header must follow

Measured with Slang reflection and SPIR-V `Offset` decorations [V]:

- Nested structs in a constant buffer start on 16-byte boundaries on HLSL and std140 SPIR-V, not
  on Metal: `Root{Heaps{Addr,Addr},Addr,Addr,uint}` puts `count` at 32 (Metal), 56 (HLSL), 64
  (std140). `-fvk-use-scalar-layout` fixes SPIR-V only, so root blocks stay flat.
- `uint3`/`float3` takes 16 bytes on Metal in constant buffers, 12 elsewhere: no 3-vectors.
- Data behind an `Addr` uses natural layout on every target (`packed_float3` on Metal).

## 6. No Graphics API concepts on Metal 4

Cost scale as in 01: native, cheap, costly, impossible.

| Concept | Metal 4 mapping | Cost | Slang lowering |
|---|---|---|---|
| `gpuMalloc`, CPU and GPU pointer | `Shared` buffer or placement-heap buffer: `contents`, `gpuAddress`; add to residency set | native (residency commit is the cost) | `Addr` → `device T*` |
| 64-bit pointers in shared structs | `gpuAddress` values in memory | native | `Addr`; raw `T*` is Metal/SPIR-V only |
| One root pointer per draw | `setAddress(root, 0)` on the argument table | cheap (a table write per draw) | `ConstantBuffer<Root>` at buffer 0 |
| Push data | none inline; frame-arena block plus address | cheap (already `bindFrameData`) | `[[vk::push_constant]]` → buffer 0 |
| 32-bit texture heap index, ranges | CPU-written 8-byte ID table, or contiguous view-pool IDs | cheap (one extra load); arithmetic form undocumented | `fetchTexture` target switch |
| Samplers in a heap | ID table of `supportArgumentBuffers` samplers; inline `constexpr` samplers count toward limits [a6] | cheap | `fetchSampler` |
| GPU-written raw descriptors | GPU can copy handle values; cannot create one | impossible to create | none |
| Barrier(producer stage, consumer stage), no states | consumer or producer queue barrier, or intra-pass barrier; nothing tracked | native | none |
| Split barrier on a memory value | events and fences only | impossible as specified | none |
| Minimal PSO, dynamic depth-stencil | depth-stencil object and raster state on the encoder; unspecialised colour state | native, cheap to specialise | `[numthreads]` → required threadgroup size |
| Static constants at pipeline creation | function constants (scalars) | cheap; pointers impossible | `[SpecializationConstant]` → `function_constant` |
| Multi-draw indirect with count | ICB plus GPU-written execution range | costly (ICB encoding) | none |
| Ray tracing | inline intersector; no SBT pipeline | cheap inline; pipeline costly | inline queries only |
| Residency | mandatory sets | cheap if batched; every pointer target must be in one | none |

## 7. Risks

1. **D3D12 pointer emulation.** Each `Addr` base costs a descriptor fetch, divergent bases need
   `NonUniformResourceIndex`, and every allocation needs a raw UAV; headers carry `Addr`, not VAs.
2. **Undocumented ID arithmetic.** `baseResourceID + i` works on M3 Max only by observation; the
   documented fallback, a CPU-written ID table, costs one load and 8 bytes per texture.
3. **Slang Metal bindless is young and fails silently.** Handle pointers in structs, unsized
   arrays and combined samplers break; HLSL pointer text and the Metal entry-point indirection pass
   Slang without error. Keep a compile-and-run test of the exact ABI on every Slang upgrade.
4. **Layout drift** between Metal natural, HLSL `cbuffer` and std140 is held only by the flat-root
   and no-3-vector rules plus a reflection-based layout check.
5. **Upgrade semantics.** `SV_InstanceID` changed meaning on Metal after the pin; re-check
   `InstanceIndex.slang` when the pin moves. Xcode 26.6 rejects `-std=metal4.1` [V], so MSL 4.1
   needs the macOS 27 toolchain (#12096, #12325).

## 8. Open and unverified items

- Intra-pass render barriers with Fragment on the after side under Metal 4 [U].
- Whether `MTLResourceID` stays an index on Apple10 (M5) [U]; only Apple9 was run.
- DXIL from the section 5 ABI was not validated with DXC; MSL 4.1 was read, not compiled [U].
- The GPU cost of the ID-table load against D3D12's heap fetch is unmeasured [U].

## Sources (accessed 2026-10-11)

- [a1] metal-cpp `release/metal-cpp_macOS27_iOS27`, <https://github.com/apple/metal-cpp>
- [a2] Metal Shading Language Specification 4.1 (2026-06-04), sections 1.3, 2.13, 5.9,
  <https://developer.apple.com/metal/Metal-Shading-Language-Specification.pdf>
- [a3] <https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api>
- [a4] MacOSX 26.5 SDK headers `MTL4CommandEncoder.h`, `MTL4ArgumentTable.h`,
  `MTL4RenderCommandEncoder.h`, `MTLTextureViewPool.h`, `MTL4Compiler.h`
- [a5] <https://support.apple.com/en-us/102894>
- [a6] Metal Feature Set Tables (2026-05-21),
  <https://developer.apple.com/metal/Metal-Feature-Set-Tables.pdf>
- [a7] <https://developer.apple.com/documentation/metal/resource-synchronization>
- [a8] WWDC25 "Explore Metal 4 games", <https://developer.apple.com/videos/play/wwdc2025/254/>;
  WWDC25 "Go further with Metal 4 games", <https://developer.apple.com/videos/play/wwdc2025/211/>
- [a9] <https://developer.apple.com/wwdc26/guides/metal/>,
  <https://developer.apple.com/videos/play/wwdc2026/359/>
- [s1] Slang user guide, convenience features (pointers, `DescriptorHandle`),
  <https://github.com/shader-slang/slang/blob/v2026.19/docs/user-guide/03-convenience-features.md>
- [s2] Slang releases, <https://github.com/shader-slang/slang/releases>
- [s3] Slang user guide `a2-02-metal-target-specific.md` and `09-targets.md` (same tree as [s1])
- [s4] Slang issues #10675, #11540, #11970, #12096, #12325, #12544 in [s2]'s repository
- [s5] slang-rhi Metal backend and pending bindless PR #802,
  <https://github.com/shader-slang/slang-rhi>
- [s6] Slang changes v2026.14.1 to v2026.19 (#12884 `SV_InstanceID`, #12885-#12887 mesh shaders),
  <https://github.com/shader-slang/slang/compare/v2026.14.1...v2026.19>
- [d1] <https://devblogs.microsoft.com/directx/shader-model-6-10-agilitysdk-720-preview/>
- [n1] S. Aaltonen, "No Graphics API", <https://www.sebastianaaltonen.com/blog/no-graphics-api>
