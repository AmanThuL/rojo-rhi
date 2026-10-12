# Engine RHI deep dives: Wicked Engine, Filament, O3DE Atom and Granite

**Status**: Proposed

Companion to the [engine RHI survey](03-engine-rhis.md), which holds the comparison table and
the cross-cutting lessons. This file holds the deep dives for Wicked Engine, Google Filament, O3DE
Atom and Granite. Method, dates and verification markers are as described there.

## 1. Wicked Engine

**Shape.** One abstract `wi::graphics::GraphicsDevice` with a header and source pair per backend.
Resources are plain description structs plus an opaque `std::shared_ptr<void> internal_state`
with frame-delayed destruction. Command lists are per-frame values from
`BeginCommandList(QUEUE_TYPE)`; `SubmitCommandLists()` submits everything begun this frame and
advances the frame, and `WaitCommandList` expresses cross-queue dependencies. Creation is
synchronous, with uploads completed at the next submit. The author's rule is "not exposing anything
from the GraphicsDevice that can be implemented on a higher level".

**Backends.** D3D12 (feature level 11_0 to 12_2, SM 6.0, SM 6.6 heap indexing), Vulkan 1.3 with
dynamic rendering, descriptor indexing, buffer device address and synchronization 2, and a native
**Metal 4** backend built on metal-cpp that refuses older devices (`"Metal 4 graphics is not
supported by your system, exiting!"`). The Metal backend uses `MTL4::CommandQueue`, one
`MTL4::CommandAllocator` per buffered frame, `MTL4::ArgumentTable`, `MTL::ResidencySet`,
`MTL4::CounterHeap` and `MTL::SharedEvent`, and compiles HLSL 6.6 to DXIL and then to metallib
with the Metal Shader Converter. DX11 was removed.

**Binding.** Hybrid. A D3D11-style slot table (14 CBV, 16 SRV, 16 UAV, 8 samplers) coexists with
bindless indices from `GetDescriptorIndex` into a 500,000-entry resource heap and 256-entry
sampler heap. On D3D12 a single root signature, written in HLSL as
`WICKED_ENGINE_DEFAULT_ROOTSIGNATURE`, is shared by every shader: directly indexed heaps, twelve
root constants, three root CBVs, a table of slot descriptors, a sampler table and ten static
samplers. On Metal the same layout is mirrored as the converter's top-level argument buffer:

```cpp
struct RootLayout { uint32_t constants[PUSH_CONSTANT_COUNT]; MTL::GPUAddress root_cbvs[ROOT_CBV_COUNT];
                    MTL::GPUAddress resource_table_ptr; MTL::GPUAddress sampler_table_ptr; };
```

with descriptor heaps as `MTL::Buffer`s of `IRDescriptorTableEntry`. Static samplers are emulated
because the converter does not support them.

**Barriers and residency.** `GPUBarrier` has memory, image (layout before and after), buffer
(state before and after) and aliasing variants; each resource has a default layout that code
leaves and returns to. D3D12 and Vulkan defer and deduplicate. On Metal 4 every barrier collapses
into one stage barrier at flush (`barrierAfterStages(MTL::StageAll, MTL::StageAll, ...)`), image
layouts are ignored, and only aliasing sets `MTL4::VisibilityOptionResourceAlias`. Every allocation
joins one `MTL::ResidencySet`, attached to the queue and committed per submit; aliasing and sparse
use placement heaps.

**Pipelines and passes.** `CreatePipelineState` without render pass information compiles lazily
per render-pass format set, keyed by the combined hash; compilation runs on the recording thread
into per-command-list lists merged at submit. Render passes are mandatory, with suspend and resume
flags for passes spanning command lists. There is no render graph; the renderer places barriers.

**Lessons.** The 2021 abstraction post calls integer command lists "rigid" and notes that
synchronous creation blocks asynchronous bindless use; DX11 SRV and UAV auto-unbinding was a
constant bug source. The bindless post records Vulkan limits on bindless constant buffers forcing
`ByteAddressBuffer`, silent bugs from a missing `NonUniformResourceIndex`, and GPU hangs from null
descriptors. The Metal backend still lacks ray-tracing pipelines and predication ("This is not
supported in Metal API"), and the converter needs an Apple developer account to download.

```cpp
// https://github.com/turanszkij/WickedEngine/blob/master/WickedEngine/wiGraphicsDevice.h
virtual bool CreatePipelineState(const PipelineStateDesc* desc, PipelineState* pso,
                                 const RenderPassInfo* renderpass_info = nullptr) const = 0;
virtual int GetDescriptorIndex(const GPUResource* resource, SubresourceType type,
                               int subresource = -1) const = 0;
virtual CommandList BeginCommandList(QUEUE_TYPE queue = QUEUE_GRAPHICS) = 0;
virtual void WaitCommandList(CommandList cmd, CommandList wait_for) = 0;
virtual void Barrier(const GPUBarrier* barriers, uint32_t numBarriers, CommandList cmd) = 0;
virtual void PushConstants(const void* data, uint32_t size, CommandList cmd, uint32_t offset = 0) = 0;
```

## 2. Google Filament

**Shape.** The backend API is generated from one file, `DriverAPI.inc`, about 150 entries declared
with macros for asynchronous commands, handle-returning commands and synchronous queries. The file
is expanded into `Driver` virtuals, into `CommandStream` methods that serialize commands into a
circular buffer, and into a dispatcher table. The engine thread records; a dedicated driver thread
executes. Handle-returning calls allocate the handle synchronously on the caller's thread and
queue the real construction, so handles are usable immediately:

```cpp
// https://github.com/google/filament/blob/main/filament/backend/include/private/backend/CommandStream.h
RetType result = mDriver.methodName##S();
using Cmd = COMMAND_TYPE(methodName##R);
void* const p = allocateCommand(sizeof(Cmd));
new(p) Cmd(mDispatcher.methodName##_, RetType(result), APPLY(std::move, params));
```

**Backends.** OpenGL (feature levels mapping to ES 2.0, 3.0 and 3.1), Vulkan 1.1, Metal (default on
Apple, Objective-C++), WebGPU through Dawn and browsers, and no D3D12; Windows uses Vulkan or GL.

**Resources and binding.** 32-bit typed handles from a fixed arena, every create call tagged.
Objects are high-level and GL-like (buffer objects with a binding type, render primitives,
render targets), and uploads are by value through descriptors that own a release callback; there
is no user-visible memory or address. Descriptor sets replaced GL-style binding in 1.55, which
required a new material version. Sampler shape is encoded in the descriptor type for WebGPU and
Metal. On Metal each set becomes an argument buffer per stage with cached argument encoders, and
residency goes through `useResources:`.

**Barriers and pipelines.** The API has no barriers. Vulkan textures keep a range map of
subresource layouts and transition at upload, sampling and attachment boundaries; Metal relies on
automatic hazard tracking. There is no pipeline object: a POD `PipelineState` is passed at bind
time and hashed lazily by each backend.

**Graph and shaders.** The FrameGraph lives above the backend, declares virtual resources with
usage bits, culls, creates and destroys transients through a cache and derives render-pass discard
and clear flags; it emits no barriers. Materials are compiled offline: GLSL through glslang to
SPIR-V and spirv-opt, then SPIRV-Cross to ESSL and MSL and Tint to WGSL.

**Lessons.** No first-party retrospective was found. The API itself records the history:
`blitDEPRECATED`, three draw entry points, `resetState()` "if the driver does any state tracking
(e.g. GL)", and a material-format break to move binding models.

```cpp
// https://github.com/google/filament/blob/main/filament/backend/include/private/backend/DriverAPI.inc
DECL_DRIVER_API_TAGGED_R_N(backend::DescriptorSetLayoutHandle, createDescriptorSetLayout,
    backend::DescriptorSetLayout&&, info)
DECL_DRIVER_API_N(bindDescriptorSet, backend::DescriptorSetHandle, dsh, backend::descriptor_set_t, set,
    backend::DescriptorSetOffsetArray&&, offsets)
DECL_DRIVER_API_N(beginRenderPass, backend::RenderTargetHandle, rth, const backend::RenderPassParams&, params)
DECL_DRIVER_API_N(bindPipeline, backend::PipelineState const&, state)
DECL_DRIVER_API_N(dispatchCompute, backend::ProgramHandle, program, math::uint3, workGroupCount)
```

## 3. O3DE Atom

**Shape.** Atom splits into the RHI and the RPI (passes, materials, shaders, scenes). Backends are
separate gems (DX12, Vulkan, Metal, Null). Akio Gaule's Vulkanised 2024 talk states the principle:
"Frame structure must be declared beforehand … Passes encapsulate all graphics work … All resource
synchronization is done under the hood by the engine. No API for direct synchronization." Every
public object is a multi-device object with a device mask fanning out to per-device objects.

**Backends.** DX12 on legacy resource states with D3D12MA, aliased heaps, ExecuteIndirect, a
bindless heap and DXR; Vulkan on VMA after the in-house allocator "performed poorly with many
entities"; Metal in Objective-C++, asserting argument buffer tier 1 ("Atom needs Argument buffer
support to run") and tier 2 for unbounded arrays, with placed resources on `MTLHeap` and no
residency sets.

**Resources and binding.** Resources are initialized through pools (`BufferPool`, `ImagePool`,
`StreamingImagePool`, `TransientAttachmentPool`) that own heap policy, and creation returns a
`ResultCode`. Binding uses Shader Resource Groups, named frequency-based groups declared in AZSL
and laid out offline by the AZSLc compiler. Updates go through `ShaderResourceGroupData` and a
deferred, batched `Compile`. Each SRG is one root descriptor table on DX12, one descriptor set on
Vulkan and one argument buffer on Metal; bindless is one special SRG. On Android, where only about
four descriptor sets are available, AZSLc merges SRGs.

**Barriers and graph.** The FrameGraph, its builder, a per-backend compiler and an executer live
inside the RHI. RPI passes act as scope producers. The compiler rebuilds the graph every frame,
assigns scopes to graphics, compute and copy queues, allocates transients into aliased heaps,
derives all barriers including cross-queue ownership and aliasing barriers, and turns barriers
into subpass dependencies where possible. Attachment use declares access, usage and stage; the
stage-less overload is deprecated, so coarse barriers proved too conservative. On Metal, resources
are hazard-untracked and collected per SRG into `useResource` calls plus `useHeap` per scope.

**Pipelines and recording.** `PipelineState` with an optional `PipelineLibrary` (the serialized
driver cache), and a thread-safe deduplicating `PipelineStateCache`. Work is submitted as data:
`DrawItem`, `DispatchItem`, `CopyItem`, where a draw item is a POD of pipeline, SRGs, root
constants and geometry. The executer splits scopes into groups by platform job policy, using
parallel render encoders on Metal.

**Lessons.** Mobile set limits forced SRG merging; the custom Vulkan allocator was replaced; the
DX12 backend still uses legacy states; coarse stage barriers were replaced by stage-precise ones.

```cpp
// https://github.com/o3de/o3de/blob/development/Gems/Atom/RHI/Code/Include/Atom/RHI/
virtual void Submit(const DeviceDrawItem& drawItem, uint32_t submitIndex = 0) = 0;   // CommandList.h
ResultCode UseAttachment(const ImageScopeAttachmentDescriptor& descriptor, ScopeAttachmentAccess access,
                         ScopeAttachmentUsage usage, ScopeAttachmentStage stage);    // FrameGraphInterface.h
virtual void SetupFrameGraphDependencies(FrameGraphInterface frameGraph) = 0;        // ScopeProducer.h
ResultCode Init(MultiDevice::DeviceMask deviceMask, const PipelineStateDescriptor& descriptor,
                PipelineLibrary* pipelineLibrary = nullptr);                         // PipelineState.h
void Compile(const ShaderResourceGroupData& shaderResourceGroupData,
             CompileMode compileMode = CompileMode::Async);                          // ShaderResourceGroup.h
```

## 4. Granite

**Shape.** Hans-Kristian Arntzen's Vulkan-only engine aims for a "mid-level abstraction": GL and
D3D11 are "too high-level and inflexible", Vulkan and D3D12 "tend to overshoot in low-level
complexity", and the target is "the good old 80/20 rule". Vulkan types appear in the public API,
so it is not portable, but it is the best-documented design of its kind. It requires Vulkan 1.1 and
uses up to 1.4.

**Resources and lifetime.** Intrusively refcounted handles for images, buffers and command
buffers, deferred deletion through two or three frame contexts that each own fences, per-thread
per-queue pools and deletion queues, and a buddy-style allocator over 64 MB chunks. Command
buffers "must be recorded and submitted in the same frame context you requested".

**Binding.** GL-like slots, four sets of sixteen bindings (the mobile minimum), layouts from SPIR-V
reflection ("using reflection to automatically generate layouts is a good idea"), descriptor sets
hashed into a temporal map so updates are rare, a linear allocator for constants, and an explicit
bindless pool.

**Barriers.** Explicit synchronization-2 barriers, ownership transfer and events, with automatic
handling only where tracking is trivial (swapchain images, transient attachments, static uploads):
"I think we have to be fairly low-level, or we inherit most of the problems with the older APIs".
The render graph owns everything else.

**Pipelines.** All render state is a packed blob hashed at draw time; render passes and
framebuffers are created lazily. "The real problem with lazily creating pipelines is
vkCreateGraphicsPipelines", which "is almost a guaranteed hitch". Fossilize captures every pipeline
creation and replays it at startup: "Pre-warming the various hashmaps in Vulkan::Device is the
strategy I used to avoid any pipeline compilation stutters."

**Graph.** Above the backend: passes declare colour, texture and storage inputs and outputs, then
`bake()` reorders passes, merges subpasses, aliases transients and emits barriers, semaphores and
async compute. The author found aliasing can add barriers and reduce overlap, and reported finding
async-compute bugs while writing the post.

```cpp
// https://github.com/Themaister/Granite/blob/master/vulkan/command_buffer.hpp
void set_texture(unsigned set, unsigned binding, const ImageView &view, const Sampler &sampler);
void *allocate_constant_data(unsigned set, unsigned binding, VkDeviceSize size);
void image_barrier(const Image &image, VkImageLayout old_layout, VkImageLayout new_layout,
                   VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                   VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access);
// https://github.com/Themaister/Granite/blob/master/renderer/render_graph.hpp
RenderPass &add_pass(const std::string &name, RenderGraphQueueFlagBits queue);
```
