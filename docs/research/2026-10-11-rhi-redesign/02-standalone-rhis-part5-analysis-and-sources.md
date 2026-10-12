# Standalone RHI libraries: a survey for the Metal 4 and D3D12 redesign — cross-cutting analysis and sources

**Status**: Proposed

Part 5 of 5; the summary and the list of parts are in [02-standalone-rhis.md](02-standalone-rhis.md).

## 5. Cross-cutting analysis

### 5.1 Convergent patterns, 2024 to 2026

1. **Directly indexed descriptor heaps replace descriptor sets.** D3D12 SM 6.6 heaps, Vulkan's
   `VK_EXT_descriptor_heap` (Khronos
   [announcement](https://www.khronos.org/blog/vulkan-introduces-roadmap-2026-and-new-descriptor-heap-extension))
   and Metal 4 argument buffers of resource ids now expose the same shape. NRI added a layout-free
   `DescriptorHeapInterface` in 2026; NVRHI added mutable bindless layouts; Daxa, slang-rhi and
   Dawn's resource table expose slot integers. All keep descriptor *writes* on the CPU because, as
   NRI notes, D3D12 has no GPU-side equivalent.
2. **GPU addresses are exposed, but rarely as the only handle.** NRI, NVRHI, slang-rhi, Daxa, Nabla,
   vuk and XUSG return a device address from a buffer object; only NoGraphicsAPI, loon_gpu, kiln and
   sgpu drop buffer objects entirely. The web layers (wgpu, Dawn, SDL, sokol) and DiligentCore
   expose none.
3. **Image layouts are retreating.** Vulkan's `VK_KHR_unified_image_layouts` (["So Long, Image
   Layouts"](https://www.khronos.org/blog/so-long-image-layouts-simplifying-vulkan-synchronisation)),
   Daxa's always-`GENERAL`, blade's "everything is GENERAL" and NoGraphicsAPI's layout-free barriers
   point one way; D3D12 enhanced barriers still require texture layouts and the explicit libraries
   (NRI, Zenith.NET, SharpGPU) keep a layout field or a separate transition call for that reason.
4. **Barrier vocabulary is converging on (stages, access[, layout]).** Whether per-resource (NRI,
   RPS, Nabla, SharpGPU) or global (Daxa, NoGraphicsAPI, blade), the terms are D3D12 enhanced
   barriers and Vulkan synchronization2. Legacy "resource state" enums persist in older or
   convenience layers (The Forge, DiligentCore, FlyCube, Wicked Engine, XUSG core).
5. **Automatic tracking survives only where convenience is the product.** NRI states it as a
   non-goal; FlyCube removed its tracker; NVRHI and DiligentCore made theirs optional with opt-outs;
   SDL ties its refusal of bindless to having no explicit barriers; wgpu documents tracking as a hot
   spot.
6. **Root data is small and per draw.** 128-byte push blocks (Daxa, nicegraf, the WebGPU
   "immediates" with one size for all stages), D3D12's 256-byte root signature, and on Metal 4 a GPU
   address written into an argument-table slot (NRI's `buffer(2)`, NoGraphicsAPI's slot 0).
   Per-frame bump allocators that return GPU addresses feed it.
7. **Timeline values are the only synchronisation primitive.** Fence value per submit (NRI,
   slang-rhi, wgpu-hal, FlyCube, DiligentCore), submit indices (Daxa, NVRHI), caller-owned timeline
   points (NoGraphicsAPI). Binary semaphores remain only for swapchain acquire and present on
   Vulkan.
8. **Render-pass objects are gone.** NRI's `CmdBeginRendering`, sokol's Vulkan backend, Daxa and
   NoGraphicsAPI use attachment descriptors at pass begin, matching Vulkan dynamic rendering, D3D12
   `BeginRenderPass` and Metal render pass descriptors.
9. **Slang is the shared front end of the newest designs**, either through reflection (slang-rhi,
   Daxa) or as a plain cross-compiler with C-layout root structs (NoGraphicsAPI, kiln, loon_gpu,
   Zenith.NET). The DXIL-plus-Metal-Shader-Converter route is the main alternative on Metal 4 (NRI,
   FlyCube, Wicked Engine).
10. **Metal 4 backends share three idioms**: one device-wide `MTLResidencySet` attached to every
    queue (NRI, FlyCube), argument tables fed with GPU addresses rather than per-slot `set*Buffer`
    calls, and stage-mask barriers emitted lazily at the next encoder. FlyCube's record-and-replay
    wrapper addresses Metal 4's one-commit command buffers for callers that expect re-submission;
    NRI instead defines command buffers as one-time-submit.

### 5.2 What maps best to Metal 4 and D3D12 with minimal overhead

The table rates each design choice by how directly it lowers to both native APIs; "direct" means one
native call or a constant-time translation with no hidden state.

| Area | Lowers directly to both | Needs translation or hidden state |
|---|---|---|
| Memory | Committed and placed resources with D3D12-style heap kinds (NRI `MemoryLocation`); raw GPU heaps with addresses (NoGraphicsAPI) — `MTLHeap`/`MTLBuffer.gpuAddress` and `ID3D12Heap`/`GetGPUVirtualAddress` | Hidden renaming (SDL cycling, QRhi dynamic buffers); Vulkan-style "get requirements then bind" on Metal |
| Buffers in shaders | Raw addresses (Metal pointers, D3D12 root descriptors) plus bindless indices for typed views | Per-draw buffer views or descriptor sets for every buffer |
| Textures and samplers | Directly indexed heaps written on the CPU at caller-chosen indices: D3D12 shader-visible heap, Metal 4 argument buffer of `MTLResourceID`s or `MTLTextureViewPool` | Descriptor sets and pools (D3D12 has none; NRI emulates them as heap ranges) |
| Root data | One GPU address per draw: Metal 4 `MTL4ArgumentTable::setAddress`, D3D12 root CBV (`SetGraphicsRootConstantBufferView`) | Large inline root constants (Metal has no direct equivalent; NRI uploads them to a buffer) |
| Barriers | Per-resource (stages, access) entries plus texture layouts: D3D12 enhanced barriers consume all of it; Metal 4 consumes the stage union and write bit | Legacy resource-state enums (need a mapping table); automatic tracking (per-command cost and hidden state) |
| Pipelines | Monolithic PSO with rasterisation, blend and attachment formats; viewport, scissor, stencil reference and depth bias dynamic | Implicit PSOs hashed from state (bgfx); runtime shader compilation |
| Commands | One-time-submit command buffers from per-thread allocators; begin and end rendering with attachment descriptors | Re-submittable command lists on Metal 4 (needs record and replay); render-pass and framebuffer objects |
| Sync | One timeline object per queue; submit lists wait and signal values | Binary semaphores; per-resource fences |
| Residency | One residency set per device attached to the queue, maintained at create and destroy | Per-encoder `useResource` |

Concretely:

- **NRI's interface shape is the best fit for the backend boundary.** Its barrier description is a
  superset that D3D12 enhanced barriers consume in full and Metal 4 reduces to a stage union; its
  memory locations mirror D3D12 heap types and map to `MTLStorageMode`; its submit description maps
  to `ExecuteCommandLists` plus `Signal`/`Wait` and to `MTL4CommandQueue::commit` plus shared
  events. Adopting the *vocabulary* does not require adopting NRI's breadth.
- **NoGraphicsAPI's root and heap model is the best fit for the binding surface**, because on Metal
  4 it is literally one `setAddress` and one heap binding, and on D3D12 it is one root CBV plus
  `SetDescriptorHeaps` with SM 6.6 indexing. Its global-only barrier is the one part that does not
  map: on D3D12 a texture that changes between render-target, depth and shader-resource use needs a
  layout transition naming that texture, or it has to live in `COMMON`/general layouts and give up
  compression.
- **Resource-scoped barriers answer the regression ADR 0006 measured.** The prototype's stage-pair
  barriers "could not scope to a resource". NRI and FlyCube show that keeping the resource list in
  the API is free on Metal 4 (the backend unions stages and ignores the list) while letting D3D12
  emit exactly the per-texture layout transitions it needs and letting validation check hazards per
  resource. Zenith.NET's split — a global stage barrier plus a separate texture-layout transition —
  reaches the same place with two calls.
- **Daxa's id-plus-address model maps if the version bits are dropped in release builds.** A slot
  index in a fixed global heap is exactly what D3D12 and Metal 4 shaders index; the generational
  version is a debug aid.
- **Avoid**: hidden resource renaming (breaks addresses), automatic barrier tracking in the core
  (wgpu #5766, NVRHI's opt-out), descriptor-set emulation on D3D12, Metal per-slot binding
  (`setVertexBuffer` style) on Metal 4, and runtime cross-compilation.

### 5.3 Signatures worth borrowing

Quoted verbatim from the headers named; the intent for RojoRHI is noted under each group.

**Barriers: per-resource entries, enhanced-barrier vocabulary, one batched call** (NRI `NRIDescs.h`,
`NRI.h`):

```c
NriStruct(AccessStage)       { Nri(AccessBits) access; Nri(StageBits) stages; };
NriStruct(AccessLayoutStage) { Nri(AccessBits) access; Nri(Layout) layout; Nri(StageBits) stages; };
NriStruct(GlobalBarrierDesc) { Nri(AccessStage) before; Nri(AccessStage) after; };
NriStruct(BufferBarrierDesc) { NriPtr(Buffer) buffer; Nri(AccessStage) before; Nri(AccessStage) after; };
NriStruct(TextureBarrierDesc) { NriPtr(Texture) texture; Nri(AccessLayoutStage) before; Nri(AccessLayoutStage) after; /* subresource range, planes, queues */ };
void (NRI_CALL *CmdBarrier)(NriRef(CommandBuffer) commandBuffer, const NriRef(BarrierDesc) barrierDesc);
```

Buffers carry no layout, textures do; Metal 4 lowers the union of stages; D3D12 lowers each entry.
Compare the global-only forms when no resource needs a layout change:

```cpp
void barrier(CommandBuffer* commands, Stage before, Access before_access, Stage after, Access after_access) noexcept; // NoGraphicsAPI
struct BarrierInfo { Access src_access = AccessConsts::NONE; Access dst_access = AccessConsts::NONE; };            // Daxa
public void Barrier(BarrierStages before, BarrierStages after)                                                     // Zenith.NET
```

**Directly indexed heap, CPU-written at caller indices, bound once** (NRI `NRIDescriptorHeap.h`;
NoGraphicsAPI; Dawn's resource table as the standards-track variant):

```c
Nri(Result) (NRI_CALL *WriteResourceDescriptors)(NriRef(DescriptorHeap) descriptorHeap, const NriPtr(WriteResourceDescriptorsDesc) writeDescs, uint32_t writeDescNum);
void        (NRI_CALL *CmdSetDescriptorHeap)(NriRef(CommandBuffer) commandBuffer, const NriRef(DescriptorHeap) descriptorHeap);
```

```cpp
void write_texture_descriptor(TextureDescriptorHeap* heap, uint32 index, const Texture* texture, TextureDescriptorType type, /* ... */) noexcept;
```

**One root address per draw or dispatch** (NoGraphicsAPI; loon_gpu shows the per-stage alternative):

```cpp
void draw(CommandBuffer* commands, const void* root, uint32 vertex_count, uint32 instance_count = 1, uint32 first_vertex = 0, /* ... */) noexcept;
void dispatch(CommandBuffer* commands, const void* root, uint32x3 group_count) noexcept;
void cmd_draw(CommandBuffer cmd, GpuPtr vertexDataGpu, GpuPtr fragmentDataGpu, uint32_t vertexCount, uint32_t instanceCount); // loon_gpu
```

**Per-frame allocation that returns an address** (Cauldron `DynamicBufferRing.h`; ADR 0006 already
chose this contract):

```cpp
D3D12_GPU_VIRTUAL_ADDRESS AllocConstantBuffer(uint32_t size, const void *pInitData);
```

**Resource identity as a slot plus a debug generation, with reverse lookup** (Daxa):

```cpp
struct BufferId { u64 index : ID_INDEX_BITS = {}; u64 version : ID_VERSION_BITS = {}; };
auto buffer_device_address_to_buffer(DeviceAddress address) -> Optional<BufferOffsetPair>;
```

The reverse lookup lets validation and crash diagnostics name the buffer behind a faulting or
suspicious address.

**Memory kinds mirroring D3D12 heap types; committed and placed creation** (NRI):

```c
NriEnum(MemoryLocation, uint8_t, DEVICE, DEVICE_UPLOAD, HOST_UPLOAD, HOST_READBACK);
Nri(Result) (NRI_CALL *CreatePlacedBuffer)(NriRef(Device) device, NriPtr(Memory) memory, uint64_t offset, const NriRef(BufferDesc) bufferDesc, NriOut NriRef(Buffer*) buffer);
uint64_t    (NRI_CALL *GetBufferDeviceAddress)(const NriRef(Buffer) buffer);
```

**Submission with explicit timeline values** (NRI; wgpu-hal's single-signal form is the minimum):

```c
NriStruct(FenceSubmitDesc) { NriPtr(Fence) fence; uint64_t value; Nri(StageBits) stages; };
NriStruct(QueueSubmitDesc) { const NriPtr(FenceSubmitDesc) waitFences; uint32_t waitFenceNum;
    const NriPtr(CommandBuffer) const* commandBuffers; uint32_t commandBufferNum;
    const NriPtr(FenceSubmitDesc) signalFences; uint32_t signalFenceNum; /* swap chain, present id */ };
```

```rust
unsafe fn submit(&self, command_buffers: &[&<Self::A as Api>::CommandBuffer], surface_textures: &[&<Self::A as Api>::SurfaceTexture],
                 signal_fence: (&<Self::A as Api>::Fence, FenceValue)) -> Result<(), DeviceError>;
```

**Freezing state that never changes** (NVRHI), useful as a validation hint even without tracking:

```cpp
virtual void setPermanentTextureState(ITexture* texture, ResourceStates stateBits) = 0;
```

**Published shader ABI for the Metal backend** (NRI `NRI.metal`): a fixed argument-table map (heaps
at 0 and 1, root data at 2, vertex streams from 6) and a 24-byte descriptor entry with static
asserts. Publishing the slot map as a header shared with Slang, as NRI does for MSL and kiln does
for Rust and Slang, keeps host and shader layouts from drifting.

```c
struct NriDescriptorEntry { ulong bufferAddress; ulong resourceId; ulong metadata; };
static_assert(sizeof(NriDescriptorEntry) == 24, "Descriptor ABI mismatch");
```

**Commands as data for a testable boundary** (FidelityFX `FfxInterface`): a job list with integer
resources that a backend replays is also a natural shape for a recording null backend in conformance
tests.

```c
typedef FfxErrorCode (*FfxScheduleGpuJobFunc)(FfxInterface* backendInterface, const FfxGpuJobDescription* job);
typedef FfxErrorCode (*FfxExecuteGpuJobsFunc)(FfxInterface* backendInterface, FfxCommandList commandList, FfxUInt32 effectContextId);
```

**Slang-driven binding** (slang-rhi), if a reflection path is wanted for tools or debug UIs rather
than the hot path:

```cpp
struct DescriptorHandle { DescriptorHandleType type = DescriptorHandleType::Undefined; uint64_t value; };
virtual Result setDescriptorHandle(const ShaderOffset& offset, const DescriptorHandle& handle) = 0;
```

### 5.4 Open questions this survey raises

- **One root or one per stage.** NoGraphicsAPI shares one root across graphics stages; loon_gpu
  passes one per stage, as Aaltonen's post does. Metal 4 argument tables are per stage; D3D12 root
  parameters have per-stage visibility. Which costs fewer binds on the frozen workloads?
- **Barrier surface.** NRI's single batched per-resource call, or Zenith.NET's global stage barrier
  plus a separate layout transition. Both lower to Metal 4 identically; they differ in caller
  arguments and in what D3D12 can validate.
- **Shader route.** Slang to metallib and DXIL with a shared root header (NoGraphicsAPI, kiln), or
  DXIL plus Metal Shader Converter with a generated root signature (NRI, Wicked Engine). The latter
  adds a runtime dependency and Apple's top-level argument buffer layout; the former needs Slang's
  Metal 4 output to be trusted.
- **Command-buffer reuse.** Define command buffers as one-time-submit (NRI) or support re-submission
  with record and replay (FlyCube). ADR 0006's frame-owned memory already assumes per-frame
  recording.
- **Debug identity for addresses.** A Daxa-style reverse lookup from address to buffer is the
  cheapest way to make pointer-first failures diagnosable; no surveyed D3D12 or Metal library
  provides one.

## 6. Sources

Library repositories are linked in the comparison table. Additional sources:

- NRI README and headers: <https://github.com/NVIDIA-RTX/NRI> (`Include/NRI.h`,
  `Include/NRIDescs.h`, `Include/Extensions/NRIDescriptorHeap.h`,
  `Include/Extensions/NRIWrapperMetal.h`, `Include/NRI.metal`,
  `Source/Metal/CommandBufferMetal.hpp`)
- NVRHI headers and guide: <https://github.com/NVIDIA-RTX/NVRHI> (`include/nvrhi/nvrhi.h`,
  `doc/ProgrammingGuide.md`); NVIDIA, "Writing Portable Rendering Code with NVRHI":
  <https://developer.nvidia.com/blog/writing-portable-rendering-code-with-nvrhi>
- FlyCube device and command-list interfaces:
  <https://github.com/andrejnau/FlyCube/tree/master/src/FlyCube>
- slang-rhi API and lifetime docs:
  <https://github.com/shader-slang/slang-rhi/blob/main/docs/api.md>,
  <https://github.com/shader-slang/slang-rhi/blob/main/include/slang-rhi.h>
- SDL3 GPU header and issues: <https://github.com/libsdl-org/SDL/blob/main/include/SDL3/SDL_gpu.h>,
  <https://github.com/libsdl-org/SDL/issues/11148>, <https://github.com/libsdl-org/SDL_shadercross>
- DiligentCore interfaces:
  <https://github.com/DiligentGraphics/DiligentCore/tree/master/Graphics/GraphicsEngine/interface>
- The Forge 1.64 interface and FSL defaults:
  <https://codeberg.org/The-Forge/The-Forge/src/branch/master/Common_3/Graphics>
- wgpu-hal contract: <https://github.com/gfx-rs/wgpu/blob/trunk/wgpu-hal/src/lib.rs>; gfx-hal
  retirement: <https://github.com/gfx-rs/gfx/discussions/3768>
- Daxa headers: <https://github.com/Ipotrick/Daxa/tree/master/include/daxa>
- webgpu.h and the bindless proposal: <https://github.com/webgpu-native/webgpu-headers>,
  <https://github.com/gpuweb/gpuweb/blob/main/proposals/bindless.md>
- sokol Vulkan backend and views update:
  <https://floooh.github.io/2025/12/01/sokol-vulkan-backend-1.html>,
  <https://floooh.github.io/2025/08/17/sokol-gfx-view-update.html>
- blade motivation: <https://github.com/kvark/blade/blob/main/blade-graphics/etc/motivation.md>
- FidelityFX backend interface:
  <https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/main/Kits/FidelityFX/api/internal/ffx_interface.h>
- RPS runtime:
  <https://github.com/GPUOpen-LibrariesAndSDKs/RenderPipelineShaders/tree/main/include/rps/runtime>
- XUSG Ultimate barriers: <https://github.com/StarsX/XUSG/blob/master/XUSG/Ultimate/XUSGUltimate.h>
- NoGraphicsAPI Metal notes and comparison:
  <https://github.com/sebbbi/NoGraphicsAPI/tree/main/docs>; Aaltonen, "No Graphics API":
  <https://www.sebastianaaltonen.com/blog/no-graphics-api>
- Vulkan descriptor heaps and unified layouts:
  <https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_descriptor_heap.html>,
  <https://www.khronos.org/blog/so-long-image-layouts-simplifying-vulkan-synchronisation>
- D3D12 enhanced barriers samples:
  <https://devblogs.microsoft.com/directx/directx-graphics-samples-updated-to-agility-sdk-enhanced-barriers-examples/>
- Apple, "Understanding the Metal 4 core API":
  <https://developer.apple.com/documentation/metal/understanding-the-metal-4-core-api>;
  `MTLTextureViewPool`: <https://developer.apple.com/documentation/metal/mtltextureviewpool>
