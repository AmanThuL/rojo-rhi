# Engine RHI deep dives: Unreal Engine 5, Godot 4 and SakuraEngine CGPU

**Status**: Proposed

Companion to the [engine RHI survey](03-engine-rhis.md), which holds the comparison table and
the cross-cutting lessons. This file holds the deep dives for Unreal Engine 5, Godot 4 and
SakuraEngine CGPU. Method, dates and verification markers are as described there.

## 1. Unreal Engine 5

**Shape.** UE has three layers. The renderer builds a Render Dependency Graph; RDG pass lambdas
record into `FRHICommandList`, an engine-defined deferred command stream; the RHI thread then
"translates" each recorded `FRHICommand` against a platform `IRHICommandContext`. `FDynamicRHI` is
"the interface which is implemented by the dynamically bound RHI", with `ID3D12DynamicRHI` and
`IVulkanDynamicRHI` as platform specializations. The command list hierarchy is
`FRHICommandListBase`, then `FRHIComputeCommandList`, then `FRHICommandList`, with
`FRHICommandListImmediate` and `FRHISubCommandList` below it. Submission was split in 5.x into
`RHIFinalizeContexts`, which closes contexts into platform command lists, and
`RHISubmitCommandLists`.

**Backends.** D3D12 and Vulkan at SM5 and SM6, Metal on Apple. Vulkan ray tracing reached parity
with D3D12 in 5.5, and the 5.5 notes call bindless "a requirement for full ray tracing support on
Vulkan". On Mac, 5.2 brought native Apple Silicon, 5.4 adopted official metal-cpp, SM6 with
Nanite runs on M2 and later and needs macOS 15 (beta in 5.5), and Epic raised the minimum macOS
"to set the stage for better integration with Metal Shader Converter" *(unverified: only search
snippets of the Epic tech blog were readable)*. No Metal 4 statement was found as of 5.7.

**Resources.** Every resource derives from `FRHIResource`, is intrusively refcounted and held
through `TRefCountPtr` aliases (`FBufferRHIRef`, `FTextureRHIRef`). Destructors are protected:
resources "should only be destructed through the deletion queue", which extends lifetime past GPU
use. Creation moved onto command lists in 5.x with descriptor structs
(`FRHIBufferCreateDesc`, `FRHITextureCreateDesc`), and initial data through initializers.
Transitions are first-class objects (`RHICreateTransition`, `RHIReleaseTransition`).

**Binding.** Shader parameter structs (`BEGIN_SHADER_PARAMETER_STRUCT`) are reflected into
slot-indexed parameters, uniform buffers with static slots and loose parameters, then batched per
draw in `FRHIBatchedShaderParameters`, a "collection of parameters to set in the RHI that aren't
bound to a specific shader until SetBatchedShaderParameters is called". Bindless arrived as extra
setters on the same batch (`SetBindlessTexture`, `SetBindlessResourceView`,
`SetResourceCollection`, `SetDescriptorRange`) and a tiered rollout. RDG documents that "a PSO
bind invalidates all shader bindings", so parameters are rebound after every pipeline change. The
issue tracker records bindless lifetime bugs: streamed mips not reloading (UE-240793) and
bindless-referenced textures collected while in use, fixed in 5.7 (UE-302299).

**Barriers.** Explicit transitions carry `ERHIAccess` before and after masks per subresource
(`FRHITransitionInfo : FRHISubresourceRange`), with split barriers
(`BeginTransitions`/`EndTransitions`), UAV overlap scopes and pipeline masks for async compute.
Short constructors that take only the new state imply the backend can resolve the prior state
*(unverified: D3D12RHI per-resource tracking is historical knowledge)*. In practice RDG derives
nearly all transitions from declared pass parameters.

**Pipelines.** Monolithic PSOs (`FGraphicsPipelineStateInitializer`) live in
`PipelineStateCache`. Epic's documentation says on-demand creation "can take 100 or more
milliseconds". Two systems answer it: bundled caches recorded by playing builds, and runtime
precaching (5.1+, `r.PSOPrecaching`), where each primitive precaches every PSO it could need from
material, vertex factory and mesh pass right after load, and the draw is skipped or a default
material used until it is ready. Precached PSOs are dropped from RAM after compilation and
refetched from the driver cache. Tom Looman's analysis judges coverage insufficient as late as
5.3.

**Threading.** Game thread, render thread (builds RDG and RHI lists, one frame ahead) and RHI
thread (translation). Parallel recording produces sub-lists chained in order by
`QueueAsyncCommandListSubmit`; 5.5 made parallel translation up to twice as fast.

**Graph relationship.** RDG is "an immediate-mode API which records render commands into a graph
data structure to be compiled and executed". It owns async compute fences, transient aliasing,
split barriers, culling, render pass merging, parallel recording and validation. Below it, the RHI
provides `IRHITransientResourceAllocator` with heap and page strategies and reports aliasing
overlaps so RDG can emit aliasing barriers: aliasing policy above, placement mechanics below.

**Lessons.** PSO hitching drove the largest piece of infrastructure in the stack. The parameter
model is a slot legacy wrapped in batching, and bindless was retrofitted rather than designed in.
Aaltonen's *No Graphics API* names this class of RHI as the one that grew hash maps and remapping
layers to emulate immediate-mode binding.

```cpp
// https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RHI/FRHIComputeCommandList
void Transition(TArrayView<const FRHITransitionInfo> Infos, ERHIPipeline SrcPipelines,
                ERHIPipeline DstPipelines, ERHITransitionCreateFlags);
// https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RHI/FRHICommandList
void BeginRenderPass(const FRHIRenderPassInfo& InInfo, const TCHAR* Name);
void SetGraphicsPipelineState(FGraphicsPipelineState* GraphicsPipelineState,
                              const FBoundShaderStateInput& ShaderInput, uint32 StencilRef,
                              bool bApplyAdditionalState);
void SetShaderParameters(FRHIGraphicsShader* InShader, TConstArrayView<uint8> InParametersData,
                         TConstArrayView<FRHIShaderParameter> InParameters,
                         TConstArrayView<FRHIShaderParameterResource> InResourceParameters,
                         TConstArrayView<FRHIShaderParameterResource> InBindlessParameters);
// https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RHI/FRHICommandListBase
FBufferRHIRef CreateBuffer(const FRHIBufferCreateDesc&);
// https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RHI/FDynamicRHI
FGraphicsPipelineStateRHIRef RHICreateGraphicsPipelineState(
    const FGraphicsPipelineStateInitializer&);
```

## 2. Godot 4: RenderingDevice and RenderingDeviceDriver

**Shape.** Three layers. `RenderingContextDriver` owns instance, adapters and surfaces.
`RenderingDeviceDriver` (RDD) is a thin, stateless, Vulkan-shaped virtual interface implemented
per backend. `RenderingDevice` (RD) is the API-agnostic layer: validation, `RID` lifetime, the
render graph, staging, the shader container cache and frame pacing. RD is also scriptable from
GDScript and C#. The split (PR #83452, Pedro J. Estébanez, 4.3) saved "thousands of lines of
duplicate code which will make porting to new platforms easier (e.g. Metal, WebGPU)".

**Backends.**
- **Vulkan** requires 1.0 and requests 1.2.
- **D3D12** (4.3) requests feature level 11_0, probes to 12_2, needs SM 6.0 and
  `TypedUAVLoadAdditionalFormats`, uses Enhanced Barriers when available and legacy states
  otherwise, and allocates through D3D12MA.
- **Metal** (4.4, PR #88199 by Stuart Carnie) has a floor of macOS 11 and iOS 14 and bakes MSL 3.1,
  and much of its code derives from MoltenVK. PR #114484 (2026) moved the driver from
  Objective-C to metal-cpp "consistent with other RDDs, which are pure C++", split Metal 3 objects
  from shared ones and introduced a command buffer base with Metal 3 and Metal 4 variants. The
  author reports a complete Metal 4 renderer on that branch; master declares `MTL4` types but
  ships no Metal 4 implementation yet.

**Handles.** RD hands out validated 64-bit `RID`s from `RID_Owner` pools. RDD uses typed POD IDs
that wrap a pointer cast to `uint64_t` (`DEFINE_ID(Buffer)`, `DEFINE_ID(UniformSet)`, and so on),
with backend structs from a shared paged allocator. Buffers declare `MemoryAllocationType {CPU,
GPU, GPU_MAPPABLE}` and an engine-only `BUFFER_USAGE_DYNAMIC_PERSISTENT_BIT` for persistently
mapped per-frame rings; device addresses are exposed.

**Binding.** Vulkan descriptor sets under the name "uniform sets": created against a shader and
set index, immutable, optionally from linear per-frame pools, with dynamic offsets and push
constants. On D3D12 each set is a contiguous range in the shader-visible resource and sampler
heaps; root signatures are generated per shader, one table per set, and serialized into the
shader container. Register mapping uses `set * 100000000 + binding * 100000` because "Vulkan slots
can hold entire arrays, whereas D3D ones need one slot per array element". On Metal a set becomes
a tier 2 argument buffer with a slot fallback. `command_uniform_set_prepare_for_use` exists for
backends that need per-use work: legacy D3D12 transitions and Metal residency.

**Barriers and the graph.** RDD barriers are Vulkan synchronization-1 in shape. RD contains an
acyclic graph (PR #84976, Darío Samo, 4.3): each draw or compute list is a node, per-resource
trackers record usage, texture usage changes count as writes because they imply layout
transitions, commands are sorted into levels, and barriers are emitted between levels from an
intermediate command buffer replayed into native calls. Clay John's Vulkanised 2024 talk reports
60 to 80 percent fewer barriers, about 10 percent lower frame time and a "massive improvement in
synchronization issues", at some CPU cost, tracking only mutable resources. On Metal the driver
offers `SyncMode { HazardTracking, Barriers }`; the newer default makes resources untracked,
heap-backed and members of one `MTLResidencySet`, and turns graph barriers into `MTLFence`
updates and waits between encoders.

**Pipelines.** Monolithic, bound to a render pass and subpass, with specialization constants
supplied at creation; caches serialize `VkPipelineCache` or `MTLBinaryArchive`. D3D12 has no
specialization constants, so Godot compiles SPIR-V through Mesa NIR to DXIL, records the bit
offsets of each constant, patches the DXIL at PSO creation and must re-sign it each time, which
forces shipping `dxil.dll`. The talk's verdict: "Not as efficient as Vulkan, but good enough."

**Recording.** Explicit queues, pools, primary and secondary command buffers. The graph can record
large draw lists in parallel as secondaries, though the blog notes this was disabled for an
NVIDIA bug; only one queue is used.

**Shaders.** GLSL through glslang to SPIR-V, reflected by RD, then converted per driver: SPIR-V
for Vulkan, NIR to DXIL for D3D12 (after an abandoned SPIRV-Cross to HLSL to DXC path), SPIRV-Cross
to MSL for Metal with optional metallib baking. A shader baker precompiles containers at export.

**Lessons.** From the Vulkanised talk: "Designed our abstraction around Vulkan … Vulkan style
barriers, pipelines, specialization constants, render passes", and before the graph, "hand-placed
barriers, hand-ordered renderpass order … tons of tricky synchronization errors". From the graph
blog: texture slice tracking was "the most painful omission from the initial design", and
"debugging is the weak point of this approach" because replay loses the recording context. On
Metal, PR #111976 fixed GPU crashes from misreading `useResources:`, since resources must be
resident "before encoding draw calls that may access the elements of resources through an
argument buffer", and PR #114484 replaced a hack that disabled argument buffers for dynamic
uniforms with a GPU-address ring buffer.

```cpp
// https://github.com/godotengine/godot/blob/master/servers/rendering/rendering_device_driver.h
virtual BufferID buffer_create(uint64_t p_size, BitField<BufferUsageBits> p_usage,
    MemoryAllocationType p_allocation_type, uint64_t p_frames_drawn) = 0;
virtual UniformSetID uniform_set_create(VectorView<BoundUniform> p_uniforms, ShaderID p_shader,
    uint32_t p_set_index, int p_linear_pool_index) = 0;
virtual void command_pipeline_barrier(CommandBufferID p_cmd_buffer,
    BitField<PipelineStageBits> p_src_stages, BitField<PipelineStageBits> p_dst_stages,
    VectorView<MemoryAccessBarrier> p_memory_barriers, VectorView<BufferBarrier> p_buffer_barriers,
    VectorView<TextureBarrier> p_texture_barriers,
    VectorView<AccelerationStructureBarrier> p_acceleration_structure_barriers) = 0;
virtual ShaderID shader_create_from_container(const Ref<RenderingShaderContainer> &p_shader_container,
    const Vector<ImmutableSampler> &p_immutable_samplers) = 0;
// https://github.com/godotengine/godot/blob/master/servers/rendering/rendering_device_graph.h
void end(bool p_reorder_commands, bool p_full_barriers, RDD::CommandBufferID &r_command_buffer,
    CommandBufferPool &r_command_buffer_pool);
// https://github.com/godotengine/godot/blob/master/drivers/metal/rendering_device_driver_metal.h
enum SyncMode { HazardTracking = 0, Barriers, };
```

## 3. SakuraEngine CGPU

**Shape and why C.** CGPU is a C99 API. Every object is an opaque `const` pointer generated by
`DEFINE_CGPU_OBJECT(name)`, every call is a free function `cgpu_verb_noun(Id, const
Descriptor*)`, and dispatch goes through a per-backend `CGPUProcTable` of function pointers that
each object caches (`proc_table_cache`). A missing entry is a runtime assert
(`"create_buffer Proc Missing!"`), which is how partial backends ship. The engine README lists a
procedural design and a C API as a headline feature. The engine's agent-oriented design notes give
four reasons for C: ABI stability across compilers, no virtual call (a function table rather than
a vtable), easy bindings to other languages, and controllable object size with no vtable pointer;
those notes sit in an AI-agent documentation folder, so they are weaker evidence than the
human-written README. The graphics README, in Chinese, states the deeper philosophy: CGPU is
designed bottom-up, "most concepts are taken from hardware or platform functionality", in contrast
to "top-down gfx libraries that statistically summarize APIs"; it "basically follows a lot of
WebGPU's design", "uses shader reflection to auto-create root signatures", and "rarely does
dirty-data caching in the backend, which lowers invoke cost and increases upstream freedom".

**Encoder rationale.** The same README explains its encoder design. D3D12 and Vulkan allocate
command memory from pools and want one allocator per thread and long command lists; Metal splits
work into scoped encoders. APIs that copy Metal's many-encoder model make D3D12 and Vulkan
allocator management hard. CGPU therefore locks scope at begin and end:
`cgpu_cmd_begin_render_pass` returns an encoder and `cgpu_cmd_end_render_pass` takes it back, so
one command buffer keeps one allocator while still mapping onto Metal encoders. Transfers are
recorded on the command buffer anywhere outside an open render pass.

**Backends.** `ECGPUBackend { VULKAN, D3D12, XBOX_D3D12, AGC, METAL }`. Vulkan 1.1 through volk and
VMA, with optional descriptor buffer and shader object extensions. D3D12 with legacy
`ResourceBarrier`, D3D12MA, DRED and DirectStorage. Metal is classic Objective-C with
`MTLArgumentEncoder` and `useResources:`; its proc table has no render pipeline or render pass
entries, and its barrier is a stub (`// TODO: Implement resource barrier with updateFence`).

**Resources and binding.** Explicit buffers and textures with separate views, memory pools and
aliasing, shared-handle import and export, tiled resources on a dedicated tile-mapping queue,
and graphics, compute and transfer queues. Error sentinels are encoded in handle values. Binding
is D3D12-flavoured: a `CGPURootSignature` built from shader reflection, deduplicated through a
root signature pool, plus descriptor sets updated by name or by index, and push constants
addressed by name. The source README calls D3D12 root constants a "failed design" to be patched by
rewriting HLSL; push descriptors are deferred.

**Barriers, pipelines and graph.** Barriers are manual with D3D12-style before and after states,
split barriers and queue ownership transfer as first-class fields; the hardware layer tracks
nothing. Pipelines take a root signature; an experimental shader-object path
(`cgpu_create_state_buffer`, raster and shader state encoders) maps to `VK_EXT_shader_object`.
SkrRenderGraph, a C++ layer above, runs dependency analysis, cross-queue synchronization, queue
scheduling, lifetime analysis, memory aliasing, barrier generation and bind-table construction.

**Lessons.** Doing all state tracking in the graph keeps the hardware layer cheap. Name-based
binding through reflection is convenient but costs string lookups on hot paths. A root-signature
abstraction designed before Metal left the Metal backend permanently behind.

```c
/* https://github.com/SakuraEngine/SakuraEngine/blob/engine/engine/modules/engine/graphics/include/SkrGraphics/api.h */
CGPU_API CGPUDeviceId cgpu_create_device(CGPUAdapterId adapter, const struct CGPUDeviceDescriptor* desc);
CGPU_API CGPURootSignatureId cgpu_create_root_signature(CGPUDeviceId device, const struct CGPURootSignatureDescriptor* desc);
CGPU_API CGPURenderPassEncoderId cgpu_cmd_begin_render_pass(CGPUCommandBufferId cmd, const struct CGPURenderPassDescriptor* desc);
CGPU_API void cgpu_cmd_end_render_pass(CGPUCommandBufferId cmd, CGPURenderPassEncoderId encoder);
CGPU_API void cgpu_cmd_resource_barrier(CGPUCommandBufferId cmd, const struct CGPUResourceBarrierDescriptor* desc);
CGPU_API void cgpu_update_descriptor_set(CGPUDescriptorSetId set, const struct CGPUDescriptorData* datas, uint32_t count);
CGPU_API CGPUStateBufferId cgpu_create_state_buffer(CGPUCommandBufferId cmd, const struct CGPUStateBufferDescriptor* desc);
```
