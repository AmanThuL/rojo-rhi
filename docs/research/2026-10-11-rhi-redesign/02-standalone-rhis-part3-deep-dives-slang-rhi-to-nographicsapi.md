# Standalone RHI libraries: a survey for the Metal 4 and D3D12 redesign — deep dives: slang-rhi to NoGraphicsAPI

**Status**: Proposed

Part 3 of 5; the summary and the list of parts are in [02-standalone-rhis.md](02-standalone-rhis.md).

### 3.4 slang-rhi

[slang-rhi](https://github.com/shader-slang/slang-rhi) grew out of Slang's `gfx` layer and is
maintained by the Slang project; its README warns that neither source nor binary compatibility is
guaranteed.

**Style.** COM-style C++ (`ISlangUnknown`, `ComPtr`), a C entry point `rhiGetInstance()`, and
Vulkan-like chainable descriptors (`structType`, `next`). Backends: D3D11, D3D12, Vulkan, Metal,
CUDA, WebGPU, CPU. The Metal backend is classic (`MTL::CommandBuffer`, tier 2 argument buffers,
residency sets when available). D3D12 uses root signature 1.1 and legacy barriers; Vulkan 1.1 to 1.3
with dynamic rendering, push descriptors and mutable descriptors; no async compute or copy queues
(`QueueType::Graphics` only).

**Addresses and bindless handles** are first-class, though descriptor handles are implemented only
on D3D12, Vulkan and CUDA:

```cpp
struct DescriptorHandle { DescriptorHandleType type = DescriptorHandleType::Undefined; uint64_t value; };
virtual DeviceAddress getDeviceAddress() = 0;                                                      // IBuffer
virtual Result getDescriptorHandle(DescriptorHandleAccess access, Format format, BufferRange range, DescriptorHandle* outHandle) = 0;
struct BindlessDesc { uint32_t bufferCount = 1024; uint32_t textureCount = 1024; uint32_t samplerCount = 128;
                      uint32_t combinedTextureSamplerCount = 1024; uint32_t accelerationStructureCount = 128; };
```

The handle values feed Slang's `DescriptorHandle<T>` on the shader side; memory is managed with
D3D12MA, VMA and Aaltonen's OffsetAllocator.

**Binding** is the library's distinctive idea: an `IShaderObject` is a CPU-side `ParameterBlock`
whose layout comes entirely from Slang reflection, from which root signatures and descriptor-set
layouts are synthesised. Binding a pipeline returns the root object to fill, addressed by path:

```cpp
virtual IShaderObject* bindPipeline(IComputePipeline* pipeline) = 0;   // pass encoder
virtual Result setData(const ShaderOffset& offset, const void* data, Size size) = 0;
virtual Result setBinding(const ShaderOffset& offset, const Binding& binding) = 0;
virtual Result setDescriptorHandle(const ShaderOffset& offset, const DescriptorHandle& handle) = 0;
// ShaderCursor cursor(passEncoder->bindPipeline(pipeline)); cursor["params"]["tex"].setBinding(texture);
```

**Commands and state.** Encoders write into an arena-allocated intermediate command list (about 40
command ids) that each backend translates at `finish()`. Barriers are tracked automatically per
encoder from shader-object bindings, with `setBufferState`, `setTextureState` and `globalBarrier`
hints. Submission takes timeline fence values:

```cpp
struct SubmitDesc { ICommandBuffer** commandBuffers; uint32_t commandBufferCount; IFence** waitFences; const uint64_t* waitFenceValues;
                    uint32_t waitFenceCount; IFence** signalFences; const uint64_t* signalFenceValues; uint32_t signalFenceCount; void* cudaStream; };
```

**Debug.** An RHI-level debug layer, GPU-assisted validation, Aftermath, live-object reporting and
persistent shader and pipeline caches.

**Assessment.** The reference for using Slang reflection as the source of truth for binding layout.
For RojoRHI it is a pattern library rather than a model: its Metal backend predates Metal 4 and
lacks bindless handles, and its automatic tracking and deferred command translation are the opposite
of thin and explicit.

### 3.5 SDL3 GPU

The [SDL3 GPU API](https://github.com/libsdl-org/SDL/blob/main/include/SDL3/SDL_gpu.h) is explicitly
portable-first: "It is designed so that the app won't have to branch itself by querying feature
support. If you need cutting-edge features with limited hardware support, this API is probably not
for you."

**Style and floors.** A C ABI with opaque pointers and `SDL_PropertiesID` extension bags; internally
a function-pointer device table per backend. Vulkan 1.0 with render-pass objects and sync1; D3D12
feature level 11_0 with resource binding tier 2; classic Metal with per-slot `set*Buffer` binding,
no argument buffers and no residency sets. No Metal 4 work is visible.

**Cycling.** Every write takes a `cycle` flag. Resources are containers of internal resources that
rotate when the active one is still referenced by in-flight work; "Cycling will never undefine
already bound data". This removes the most common synchronisation hazard for beginners, at the price
of the handle no longer identifying memory — a stored GPU address would go stale after a cycle.

```c
void *SDL_MapGPUTransferBuffer(SDL_GPUDevice *device, SDL_GPUTransferBuffer *transfer_buffer, bool cycle);
void SDL_UploadToGPUBuffer(SDL_GPUCopyPass *copy_pass, const SDL_GPUTransferBufferLocation *source, const SDL_GPUBufferRegion *destination, bool cycle);
```

**Barriers.** None exposed. Each resource has a default read state derived from its usage flags;
writes transition out of it and back, and compute passes declare their read-write resources at
`SDL_BeginGPUComputePass` so the backend knows what to transition. The maintainers tie bindless to
this choice: in issue [#11148](https://github.com/libsdl-org/SDL/issues/11148) they see no way to
add it "without exposing manual memory barriers which is another major design problem" and defer it
to SDL 4.

**Binding.** Fixed per-stage slots (16 sampled textures, 8 storage textures, 8 storage buffers, 4
uniform slots of up to 32 KiB) with one documented register convention per shader format (SPIR-V
sets 0–3, HLSL spaces 0–3, MSL argument indices). Uniforms are pushed and suballocated from a
per-command-buffer ring:

```c
void SDL_PushGPUVertexUniformData(SDL_GPUCommandBuffer *command_buffer, Uint32 slot_index, const void *data, Uint32 length);
bool SDL_WaitAndAcquireGPUSwapchainTexture(SDL_GPUCommandBuffer *command_buffer, SDL_Window *window, SDL_GPUTexture **swapchain_texture, Uint32 *w, Uint32 *h);
SDL_GPUFence *SDL_SubmitGPUCommandBufferAndAcquireFence(SDL_GPUCommandBuffer *command_buffer);
```

**Assessment.** The best-documented example of how far an implicit, portable design can go, and of
exactly where it stops (bindless, ray tracing, mesh shaders). Its
swapchain-texture-acquired-into-a-command-buffer pattern and its per-backend register convention are
tidy; its cycling is the clearest counter-example to an address-first design.

### 3.6 DiligentCore

[DiligentCore](https://github.com/DiligentGraphics/DiligentCore) is the core of Diligent Engine: C++
COM-style interfaces with a generated C API, backends D3D11, D3D12, Vulkan, OpenGL/GLES and WebGPU
in the open source, and a Metal backend "available for commercial clients" whose public headers show
classic Metal types.

**Binding.** A Pipeline Resource Signature declares named resources with an update frequency —
`STATIC`, `MUTABLE` or `DYNAMIC` — and a Shader Resource Binding object instantiates it. Bindless
uses runtime-sized arrays, not heap indices. Inline constants (2025) map to root or push constants
and are emulated with constant buffers on D3D11 and GL:

```cpp
VIRTUAL void METHOD(CommitShaderResources)(THIS_ IShaderResourceBinding* pShaderResourceBinding, RESOURCE_STATE_TRANSITION_MODE StateTransitionMode) PURE;
VIRTUAL void METHOD(SetInlineConstants)(THIS_ const void* pConstants, Uint32 FirstConstant, Uint32 NumConstants) PURE;
```

**State.** Every resource carries a tracked state; draw, commit and copy calls take
`RESOURCE_STATE_TRANSITION_MODE_TRANSITION` (engine inserts barriers), `_VERIFY` (debug check only)
or `_NONE`, and setting a resource's state to `UNKNOWN` opts it out. `TransitionResourceStates`
takes legacy before/after states with split-barrier support. D3D12 uses legacy barriers only; Vulkan
uses sync1.

**Other.** Immediate contexts per queue plus deferred contexts, timeline-style fences, async PSO
compilation, a PSO archiver for offline baking, no public device-address query, and no Slang
integration beyond accepting precompiled bytecode.

**Assessment.** The fullest example of the "superset" design that *No Graphics API* argues against.
Two ideas transfer: the per-variable update-frequency vocabulary, and the `VERIFY` mode — validate
that the caller's declared state is what tracking would have produced, without inserting anything.

### 3.7 The Forge

[The Forge](https://codeberg.org/The-Forge/The-Forge) moved from GitHub to Codeberg in August 2026;
release 1.64 publishes only the PC/D3D12 runtime under Apache-2.0, with other platforms commercial.
The Metal and Vulkan backends survive in the older GitHub tree (release 1.63).

**Style.** A C99 API of free functions on structs with public native fields (`initRenderer`,
`addPipeline`, `cmdBindDescriptorSet`, `queueSubmit`), all prefixed `TF` since 1.64.

**Binding.** The 1.61 "Shader Resource Tables" rework fixed the root signature to four frequency
tables — `Persistent`, `PerFrame`, `PerBatch`, `PerDraw` — plus a static-sampler space, declared
once in an `.srt.h` header included by both C++ and shaders:

```c
BEGIN_SRT(SrtData)
    BEGIN_SRT_SET(PerFrame)
        DECL_CBUFFER(PerFrame, CBUFFER(UniformData), gUniformBlock)
    END_SRT_SET(PerFrame)
END_SRT(SrtData)
```

On Metal each table becomes an argument buffer; the global root signature means pipelines carry no
layout.

**Barriers.** Explicit per-resource legacy states with split and queue-ownership flags:

```c
void cmdResourceBarrier(TFCmd* pCmd, uint32_t bufferBarrierCount, TFBufferBarrier* pBufferBarriers, uint32_t textureBarrierCount, TFTextureBarrier* pTextureBarriers, uint32_t rtBarrierCount, TFRenderTargetBarrier* pRtBarriers);
```

**Shaders.** FSL, an HLSL superset translated by a Python tool to HLSL, Vulkan GLSL, MSL and console
languages. The Visibility Buffer and GPU breadcrumbs are notable features.

**Assessment.** Its single, frequency-indexed root layout shared between host and shader is the
closest production precedent for a fixed root: pipelines need no layout and binding is "which table
changed". Its Metal backend is classic and no longer public.

### 3.8 wgpu and wgpu-hal

[wgpu](https://github.com/gfx-rs/wgpu) implements WebGPU in Rust for Firefox, Deno, Bevy and many
others. It has three layers: `wgpu` (idiomatic API), `wgpu-core` (validation, tracking, lifetime)
and `wgpu-hal` (unsafe portable layer), plus the `naga` shader translator.

**Why gfx-hal died.** The predecessor gfx-hal was a Vulkan-shaped portable API; maintaining Vulkan
semantics on D3D12 and Metal (command-pool emulation, subpasses, secondary command buffers) cost
velocity and overhead that wgpu never needed. wgpu-hal replaced it in 2021 with "60% fewer lines of
code" (22k against 55k), per the [0.10 release
post](https://gfx-rs.github.io/2021/08/18/release-0.10.html). The lesson: shape the portable layer
around the abstraction's own needs, not around the most verbose backend.

**HAL contract.** Quoted from the crate docs: no validation or tracking overhead, unsafe contracts,
static dispatch (dynamic dispatch was added as `Dyn*` traits in v24), persistent mapping, explicit
barriers, explicitly specified pipeline layouts, and errors only for conditions the caller cannot
anticipate. Barriers are per-resource usage transitions; submit signals one timeline value:

```rust
pub struct BufferBarrier<'a, B: DynBuffer + ?Sized> { pub buffer: &'a B, pub usage: StateTransition<wgt::BufferUses> }
unsafe fn transition_buffers<'a, T>(&mut self, barriers: T) where T: Iterator<Item = BufferBarrier<'a, <Self::A as Api>::Buffer>>;
unsafe fn set_immediates(&mut self, layout: &<Self::A as Api>::PipelineLayout, offset_bytes: u32, data: &[u32]);
unsafe fn submit(&self, command_buffers: &[&<Self::A as Api>::CommandBuffer], surface_textures: &[&<Self::A as Api>::SurfaceTexture],
                 signal_fence: (&<Self::A as Api>::Fence, FenceValue)) -> Result<(), DeviceError>;
```

**Backends.** Classic Metal through objc2 (the adapter detects the Metal 4 family and MSL 4.0 but
has no MTL4 command path), D3D12 at feature level 11_0 with DXC, Vulkan through ash, GL/GLES.
Bindless is "binding arrays" (Metal limits raised to 500,000 sampled textures in v25); push
constants became "immediates" in v28 with one size shared by all stages; ray query, ray-tracing
pipelines and mesh shaders are experimental. There is no public buffer device address.

**Overhead.** "Arcanization" (0.19, January 2024) moved resources behind `Arc` and cut lock scope;
Bevy reported a 45% frame-time reduction from parallel shadow encoding
([post](https://gfx-rs.github.io/2023/11/24/arcanization.html)). Issue
[#5766](https://github.com/gfx-rs/wgpu/issues/5766) records that compute passes with many read-write
resources are slow "due to barrier emitting between dispatch calls" — the cost of automatic tracking
at the core layer.

**Assessment.** wgpu-hal's contract paragraph is the clearest statement in the field of what an
explicit layer owes and does not owe its caller, and its `submit(..., (fence, value))` is a minimal
sync model. WebGPU's binding and validation model is not a fit for a thin Metal 4 + D3D12 layer.

### 3.9 Daxa

[Daxa](https://github.com/Ipotrick/Daxa) is a Vulkan 1.3-only, bindless-first library (release 3.6,
May 2026) targeting Turing, RDNA3 and Arc or newer.

**Resources are integers.** Every resource is a 64-bit id — 20 bits of slot, 44 bits of version —
indexing one global descriptor set, with use-after-free detectable from the version. Buffers carry
no usage flags (every buffer gets device-address, transfer, storage, index and indirect usage) and
memory flags are only two host-access hints:

```cpp
struct BufferId { u64 index : ID_INDEX_BITS = {}; u64 version : ID_VERSION_BITS = {}; };
auto create_buffer(BufferInfo const & info) -> BufferId;
auto buffer_device_address(BufferId buffer) const -> Optional<DeviceAddress>;
auto buffer_device_address_to_buffer(DeviceAddress address) -> Optional<BufferOffsetPair>; // reverse lookup
void destroy_buffer(BufferId buffer);   // deferred until GPU completion
```

**Shader side.** One descriptor set with fixed bindings (storage buffers, storage images, sampled
images, samplers, a device-address table indexed by `BufferId`, acceleration structures); Slang
`__DynamicResource` lets one index be any resource type, and a buffer id converts to a pointer in
the shader. Pipelines take only a push-constant size (default 128 bytes); there are no user
descriptor-set layouts.

**Barriers.** Images live in `GENERAL` permanently; the only layout operations are "to general" and
"to present". Barriers are global stage/access pairs:

```cpp
struct Access { PipelineStageFlags stages; AccessTypeFlags type; };
struct BarrierInfo { Access src_access = AccessConsts::NONE; Access dst_access = AccessConsts::NONE; };
enum struct ImageLayoutOperation { NONE = 0, TO_GENERAL = 1, TO_PRESENT_SRC = 2 };
void pipeline_barrier(BarrierInfo const & info);
```

**Submission.** Fixed queue slots (main, four compute, two transfer); `submit_commands` takes
timeline and binary semaphores and returns a 64-bit submit index usable as a dependency. A TaskGraph
utility reorders tasks, aliases transient memory and generates shader-visible attachment blobs.

**Assessment.** The cleanest shipping example of an id-plus-address resource model, and evidence
that "no usage flags, no layouts, global barriers" is workable on desktop hardware. It is
Vulkan-only; its "always GENERAL" stance has no D3D12 equivalent without cost (see section 5.2).

### 3.10 sebbbi/NoGraphicsAPI

[NoGraphicsAPI](https://github.com/sebbbi/NoGraphicsAPI) is Aaltonen's reference implementation of
his *No Graphics API* post, on Vulkan 1.4 (`VK_EXT_descriptor_heap`,
`VK_KHR_device_address_commands`, `VK_KHR_shader_untyped_pointers`) and Metal 4 (macOS 26, Apple
family 7). [01-nographicsapi.md](01-nographicsapi.md) analyses it; the surface relevant to this
comparison is:

```cpp
[[nodiscard]] GpuHeap create_gpu_heap(Device* device, uint64 byte_count, MemoryType memory = MemoryType::cpu_visible) noexcept;
void write_texture_descriptor(TextureDescriptorHeap* heap, uint32 index, const Texture* texture, TextureDescriptorType type, /* ... */) noexcept;
void barrier(CommandBuffer* commands, Stage before, Access before_access, Stage after, Access after_access) noexcept;
void draw(CommandBuffer* commands, const void* root, uint32 vertex_count, uint32 instance_count = 1, uint32 first_vertex = 0, /* ... */) noexcept;
void dispatch(CommandBuffer* commands, const void* root, uint32x3 group_count) noexcept;
void submit(Device* device, const SubmitDesc& desc, uint32 queue_index = 0) noexcept;
```

There are no buffer objects; heaps expose CPU and GPU addresses that the application partitions.
Textures are placed in GPU-only texture heaps; texture and sampler descriptors live at
application-chosen indices (on Metal, an `MTLTextureViewPool` so the shader computes `base +
index`). Each draw takes one root pointer shared by all graphics stages, placed in argument-table
slot 0 on Metal. PSOs have no binding or vertex layout. Creation returns an error or a null handle;
frame operations return nothing and are checked by assertions — the same split as ADR 0003.
