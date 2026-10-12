# Standalone RHI libraries: a survey for the Metal 4 and D3D12 redesign — deep dives: NRI, NVRHI and FlyCube

**Status**: Proposed

Part 2 of 5; the summary and the list of parts are in [02-standalone-rhis.md](02-standalone-rhis.md).

## 3. Deep dives

### 3.1 NVIDIA NRI

[NRI](https://github.com/NVIDIA-RTX/NRI) (NVIDIA Render Interface, version 190) calls itself "a
Graphics API, not RHI!" in `NRI.h`. Its non-goals are exposing entities that do not exist in the
native APIs, D3D11-like abstraction, hidden management, and automatic barriers ("better handled in a
higher-level abstraction"). It is the integration layer for NVIDIA's NRD and upscaler SDKs.

**API style.** One C/C++ header set (`NriStruct`, `NriRef` macros select C or C++). Functionality is
split into versioned interfaces obtained at run time — `CoreInterface`, `DescriptorHeapInterface`,
`RayTracingInterface`, `MeshShaderInterface`, `SwapChainInterface`, `StreamerInterface`,
`UpscalerInterface`, `LowLatencyInterface`, `ImguiInterface`, `WrapperMetalInterface` and others —
each a struct of function pointers:

```c
NRI_API Nri(Result) NRI_CALL nriGetInterface(const NriRef(Device) device, const char* interfaceName, size_t interfaceSize, void* interfacePtr);
// Example: Result result = nriGetInterface(device, NRI_INTERFACE(CoreInterface), &coreInterface)
```

The function table makes the virtual-call cost explicit and lets the validation backend interpose.
Thread-safety is documented once: `Create*` thread-safe, `Destroy*` and `Cmd*` not. The README
claims "low overhead with zero memory allocations at runtime, honored user-provided memory
allocator"; no measurement is published.

**Backends.** D3D12 (floor: D3D12 Ultimate, Windows SDK 10.0.20348, Agility SDK recommended), Vulkan
(1.2 core plus `VK_KHR_synchronization2`; uses dynamic rendering, `VK_EXT_descriptor_heap`,
`VK_KHR_unified_image_layouts`, `VK_KHR_shader_untyped_pointers` and many more when present), D3D11,
WebGPU through wgpu-native, Metal through MoltenVK, a dummy backend, and **native Metal 4 through
metal-cpp** (macOS 26 / iOS 26, Xcode 26). The Metal 4 backend accepts native metallib shaders
following the ABI in `NRI.metal`, or DXIL converted at run time or offline by Apple's Metal Shader
Converter 4.0.1 against a root signature NRI generates:

```c
// Converter root signature of a pipeline layout as "metal-shaderconverter --root-signature" JSON
Nri(Result) (NRI_CALL *GetRootSignatureMetal)(const NriRef(PipelineLayout) pipelineLayout, NriOut char* json, NonNriRef(uint64_t) size);
NRI_API Nri(Result) NRI_CALL nriCreateDeviceFromMetalDevice(const NriRef(DeviceCreationMetalDesc) deviceDesc, NriOut NriRef(Device*) device);
```

**Memory.** Both models are offered side by side. Vulkan-style: create the resource,
`Get*MemoryDesc`, `AllocateMemory`, `Bind*Memory`. D3D12-style: `CreateCommittedBuffer` or
`CreatePlacedBuffer`, with a four-value heap kind that mirrors D3D12 heap types; passing a heap
macro instead of a memory object suballocates through AMD's allocator:

```c
NriEnum(MemoryLocation, uint8_t, DEVICE, DEVICE_UPLOAD, HOST_UPLOAD, HOST_READBACK);
Nri(Result) (NRI_CALL *CreateCommittedBuffer)(NriRef(Device) device, Nri(MemoryLocation) memoryLocation, float priority, const NriRef(BufferDesc) bufferDesc, NriOut NriRef(Buffer*) buffer);
Nri(Result) (NRI_CALL *CreatePlacedBuffer)(NriRef(Device) device, NriPtr(Memory) memory, uint64_t offset, const NriRef(BufferDesc) bufferDesc, NriOut NriRef(Buffer*) buffer);
uint64_t    (NRI_CALL *GetBufferDeviceAddress)(const NriRef(Buffer) buffer); // "0" if unsupported
void*       (NRI_CALL *MapBuffer)(NriRef(Buffer) buffer, uint64_t offset, uint64_t size); // persistent on D3D12/VK
```

On Metal 4 every resource joins one device-wide `MTLResidencySet` that is attached to every queue,
so residency is invisible to the caller.

**Binding.** The core model is a root signature in all but name: a `PipelineLayoutDesc` of root
constants, root descriptors, root (static) samplers and descriptor sets of ranges, with the D3D12
256-byte root budget published as a device limit. Descriptor sets are tiny (at most 48 bytes) and
allocated from pools. In 2026 a separate layout-free interface was added for true bindless:

```c
// Goal: true bindless resource access through directly indexed descriptor heaps
NriStruct(DescriptorHeapDesc) { uint32_t resourceDescriptorNum; uint32_t samplerDescriptorNum; };
NriStruct(WriteResourceDescriptorsDesc) { const NriPtr(Descriptor) resource; uint32_t descriptorIndex; };
Nri(Result) (NRI_CALL *CreateDescriptorHeap)(NriRef(Device) device, const NriRef(DescriptorHeapDesc) descriptorHeapDesc, NriOut NriRef(DescriptorHeap*) descriptorHeap);
Nri(Result) (NRI_CALL *WriteResourceDescriptors)(NriRef(DescriptorHeap) descriptorHeap, const NriPtr(WriteResourceDescriptorsDesc) writeDescs, uint32_t writeDescNum);
void        (NRI_CALL *CmdSetDescriptorHeap)(NriRef(CommandBuffer) commandBuffer, const NriRef(DescriptorHeap) descriptorHeap);
// GPU-side updates are not supported because D3D12 has no equivalent functionality
```

On Metal 4 the heaps are buffers of 24-byte entries and the root data is one buffer whose GPU
address is placed in an argument-table slot, per `NRI.metal`:

```c
// Argument table: buffer(0) resource heap, buffer(1) sampler heap, buffer(2) root data, buffer(6 + N) vertex stream N
struct NriDescriptorEntry { ulong bufferAddress; ulong resourceId; ulong metadata; };
// Root data: root constants tightly packed, then root descriptors (GPU addresses) aligned to 8 bytes,
// then per descriptor set a pointer to its resource table and its sampler table, padded to 16 bytes
```

**Barriers.** Enhanced-barrier vocabulary throughout, with global, buffer and texture barriers
batched in one call; layouts apply to textures only:

```c
NriStruct(AccessStage)       { Nri(AccessBits) access; Nri(StageBits) stages; };
NriStruct(AccessLayoutStage) { Nri(AccessBits) access; Nri(Layout) layout; Nri(StageBits) stages; };
NriStruct(BufferBarrierDesc) { NriPtr(Buffer) buffer; Nri(AccessStage) before; Nri(AccessStage) after; };
NriStruct(TextureBarrierDesc) { NriPtr(Texture) texture; Nri(AccessLayoutStage) before; Nri(AccessLayoutStage) after;
    Nri(Dim_t) mipOffset; Nri(Dim_t) mipNum; Nri(Dim_t) layerOffset; Nri(Dim_t) layerNum; Nri(PlaneBits) planes;
    NriOptional NriPtr(Queue) srcQueue; NriOptional NriPtr(Queue) dstQueue; };
NriStruct(BarrierDesc) { const NriPtr(GlobalBarrierDesc) globals; uint32_t globalNum;
    const NriPtr(BufferBarrierDesc) buffers; uint32_t bufferNum; const NriPtr(TextureBarrierDesc) textures; uint32_t textureNum; };
void (NRI_CALL *CmdBarrier)(NriRef(CommandBuffer) commandBuffer, const NriRef(BarrierDesc) barrierDesc);
```

The Metal 4 implementation (`CommandBufferMetal.hpp`) unions the before and after stages of all
entries, uses `MTL4::VisibilityOptionDevice` only when some `before.access` is a write
("Read-to-write (WAR) dependencies need only an execution barrier"), defers the barrier to the next
encoder when none is open ("saving an encoder which would contain only barriers"), and inside a
render pass splits it into `barrierAfterEncoderStages` for pre-rasterisation work and
`barrierAfterQueueStages` for earlier encoders. This is a worked proof that a per-resource API costs
nothing on Metal 4.

**Pipelines and commands.** Monolithic graphics, compute and ray-tracing PSOs; viewport and scissor
mandatory dynamic state, stencil reference, depth bounds, blend constants, sample locations, shading
rate and depth bias dynamic where supported. Rendering uses `CmdBeginRendering`/`CmdEndRendering`
with attachment descriptors, no render-pass objects. Command allocators are created per queue,
command buffers are documented as one-time-submit, and recording is single-threaded per command
buffer. Draws take descriptor structs (`CmdDraw(const DrawDesc&)`).

**Submission and sync.** Fences are timeline (`ID3D12Fence`, `VkSemaphore` timeline,
`MTLSharedEvent`); each submit lists wait and signal fence values with stage masks:

```c
NriStruct(FenceSubmitDesc) { NriPtr(Fence) fence; uint64_t value; Nri(StageBits) stages; };
NriStruct(QueueSubmitDesc) { const NriPtr(FenceSubmitDesc) waitFences; uint32_t waitFenceNum;
    const NriPtr(CommandBuffer) const* commandBuffers; uint32_t commandBufferNum;
    const NriPtr(FenceSubmitDesc) signalFences; uint32_t signalFenceNum;
    NriOptional const NriPtr(SwapChain) swapChain; NriOptional uint64_t presentId; };
```

**Debug.** An NRI validation backend (`enableNRIValidation`), GAPI validation, Vulkan printf,
NVTX/PIX annotations, debug names on every object, native-object getters, and wrapper interfaces
that import native D3D12, Vulkan or Metal objects.

**Assessment.** NRI has already solved the problem RojoRHI's redesign poses — one explicit interface
over D3D12 and Metal 4 — and published its Metal ABI. Its weaknesses for RojoRHI are breadth (eleven
interfaces, D3D11 and WebGPU compromises such as two memory models) and a binding model that still
centres on pipeline layouts and descriptor sets, with the directly indexed heap added beside it
rather than replacing it.

### 3.2 NVRHI

[NVRHI](https://github.com/NVIDIA-RTX/NVRHI) is NVIDIA's convenience layer over D3D11, D3D12 and
Vulkan 1.3, used by Donut and most RTX SDK samples. NRI's README positions the two: NVRHI "offers a
D3D11-like abstraction layer, implying some overhead". There is no Metal backend.

**Style and resources.** C++ abstract interfaces with intrusive `RefCountPtr` handles; resources are
reference-counted and destruction is deferred until command lists retire (`runGarbageCollection`).
Buffers expose `getGpuVirtualAddress()`. Placed resources exist (`createHeap`, `bindBufferMemory`),
sparse tiles and sampler feedback are supported.

**Binding.** `BindingLayout` (a descriptor-set or root-table layout) plus immutable `BindingSet`s;
"volatile" constant buffers are versioned by the command list from an upload ring, so `writeBuffer`
on one is cheap. Bindless is a `BindlessLayoutDesc` with a `DescriptorTable` written by index; its
`LayoutType::MutableSrvUavCbv` mode switches D3D12 to `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` and Vulkan
to DXC's `-fvk-bind-resource-heap`:

```cpp
virtual BindingLayoutHandle createBindlessLayout(const BindlessLayoutDesc& desc) = 0;
virtual DescriptorTableHandle createDescriptorTable(IBindingLayout* layout) = 0;
virtual bool writeDescriptorTable(IDescriptorTable* descriptorTable, const BindingSetItem& item) = 0;
virtual void setPushConstants(const void* data, size_t byteSize) = 0;
```

**State.** All draw-time state is one value struct, which makes redundant-state filtering trivial:

```cpp
struct GraphicsState { IGraphicsPipeline* pipeline; IFramebuffer* framebuffer; ViewportState viewport; /* ... */
    BindingSetVector bindings; static_vector<VertexBufferBinding, c_MaxVertexAttributes> vertexBuffers;
    IndexBufferBinding indexBuffer; IBuffer* indirectParams; IBuffer* indirectCountBuffer; };
virtual void setGraphicsState(const GraphicsState& state) = 0;
```

**Barriers.** Automatic tracking is on by default and can be disabled per command list. The useful
idea is the escape valves: a resource can be frozen into a permanent state and leave tracking, and
UAV-to-UAV barriers can be suppressed per resource. The D3D12 backend emits enhanced barriers when
the device supports them.

```cpp
virtual void setEnableAutomaticBarriers(bool enable) = 0;
virtual void setPermanentTextureState(ITexture* texture, ResourceStates stateBits) = 0;
virtual void setEnableUavBarriersForTexture(ITexture* texture, bool enableBarriers) = 0;
virtual void setTextureState(ITexture* texture, TextureSubresourceSet subresources, ResourceStates stateBits) = 0;
virtual void commitBarriers() = 0;
```

**Submission.** `executeCommandLists` returns a 64-bit instance number per queue;
`queueWaitForCommandList(waitQueue, executionQueue, instance)` is the cross-queue wait — a timeline
in disguise.

**Assessment.** A mature reference for what convenience costs: tracking, versioning and lifetime
management. Its `setPermanentTextureState` concept (most resources never change state after
creation) is worth keeping even in an explicit design.

### 3.3 FlyCube

[FlyCube](https://github.com/andrejnau/FlyCube) is "a Graphics API wrapper on top of DirectX 12,
Vulkan and Metal 4" by a single author, with commit history showing "Metal 4 initial support" on
2025-10-09 and "Try to implement metal 4 barriers" two days later; the Metal 3 path was dropped
because Metal 4 needs `MTLGPUFamilyApple7` or later.

**Style.** C++20 pure-virtual interfaces, every object a `std::shared_ptr`; third-party types
(`gli::format`) appear in public headers. The 2025 cleanup removed the automatic
`ResourceStateTracker` and the higher-level program and render-pass layers, leaving a thinner core.

**Memory and resources.** No allocator library: committed resources, or `AllocateMemory` plus
`CreatePlacedBuffer`. Resources do not expose GPU addresses publicly; the Metal backend internally
works in `gpuAddress + offset` for index, indirect and argument-table bindings.

**Binding.** `BindingSetLayout` from HLSL `(register, space)` keys; bindless through SM 5.1
unbounded arrays per space and a typed view pool, not SM 6.6 heaps. On Metal 4, bindless is one
global argument buffer with a residency set attached to the `MTL4CommandQueue`.

```cpp
virtual std::shared_ptr<BindlessTypedViewPool> CreateBindlessTypedViewPool(ViewType view_type, uint32_t view_count) = 0;
virtual void WriteView(uint32_t index, const std::shared_ptr<View>& view) = 0;   // BindlessTypedViewPool
virtual uint32_t GetBaseDescriptorId() const = 0;
```

**Barriers.** D3D12 legacy before/after states per resource and subresource range, plus
`UAVResourceBarrier`. D3D12 uses legacy `ResourceBarrier` at feature level 11_1; Vulkan issues
coarse all-commands barriers for layout changes; Metal 4 collapses states to stage masks and issues
`barrierAfterQueueStages` lazily before the next draw or dispatch.

**Commands.** An `MTL4CommandBuffer` can be committed once, so the Metal device wraps every command
list in `RecordCommandList<MTCommandList>`, which records calls as closures and replays them into a
fresh command buffer on resubmission — a cheap way to keep D3D12's "record once, execute many".

```cpp
virtual void ExecuteCommandLists(const std::vector<std::shared_ptr<CommandList>>& command_lists) = 0;
virtual void Signal(const std::shared_ptr<Fence>& fence, uint64_t value) = 0;
virtual uint32_t NextImage(const std::shared_ptr<Fence>& fence, uint64_t signal_value) = 0;   // Swapchain
virtual void Present(const std::shared_ptr<Fence>& fence, uint64_t wait_value) = 0;
```

**Shaders.** HLSL through DXC; Metal through SPIR-V and SPIRV-Cross by default, or DXIL and Metal
Shader Converter with explicit `IRRootSignature` layouts (the December 2025 focus), compiled with
`MTL4Compiler`.

**Assessment.** The most compact working D3D12 + Metal 4 code base to read, and the only one besides
NRI to show Metal 4 barrier lowering, argument tables and residency sets in a cross-API setting. Its
D3D12 side (feature level 11_1, legacy barriers, SM 5.1 arrays) sits below RojoRHI's intended floor.
