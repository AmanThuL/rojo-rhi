# Engine RHI catalogue: shorter entries and sources

**Status**: Proposed

Companion to the [engine RHI survey](03-engine-rhis.md). This file holds shorter entries for the
engines and frameworks not covered by a deep dive, and the source list for the whole survey.

## 1. Shorter entries

**Unity.** `GfxDevice` is closed; its shape shows through the native plugin interfaces. The D3D12
interface makes a plugin declare resource states around `ExecuteCommandList` (and v1 exposed
`GetResourceState`/`SetResourceState`), so the device tracks D3D12 states. The Vulkan interface
asks for a layout per access (`AccessTexture`, `AccessBuffer`, `EnsureOutsideRenderPass`), so the
device tracks layouts and inserts barriers. The Metal interface only hands out the current command
buffer and encoder and needs no state handshake. Graphics jobs have legacy, native and split modes;
split threading is limited to D3D12 and Vulkan. SRP RenderGraph above the device handles
lifetimes, culling, memory reuse, native render pass merging on tile-based GPUs and async compute.
Unity 6 added `GraphicsStateCollection` to trace and warm PSOs, with automatic warmup at first scene
load in 6.6. DXC is production only on D3D12.

**Frostbite FrameGraph** (Yuriy O'Donnell, GDC 2017). Setup, compile and execute phases over the
existing RenderContext (DX11, DX12, GNM, Metal, GLES, Mantle). The compiler culls, computes
lifetimes, aliases transients (147 MB to 80 MB at 720p, 1042 MB to 472 MB at 4K on DX12) and
derives bind flags. Async compute is a manual per-pass opt-in because "misuse can increase memory
use". Aliasing hazards include compression metadata and compute-to-pixel hazards not covered by
transitions. This talk is the template UE RDG, Unity and The Machinery follow.

**Traverse Research Breda.** A Rust framework over ash and windows-rs with HLSL compiled for D3D12,
Vulkan and Metal. "All pipelines share the same descriptor heap, and pipeline resource layout":
handles are 32-bit heap indices, D3D12 uses an almost empty SM 6.6 root signature with directly
indexed heaps, and Vulkan stays within four sets by using immutable samplers. "Breda aims to do as
little resource tracking as possible, therefore we heavily rely on resource promotion and decay."
Their open-source `saxaboom` wraps the Metal Shader Converter, so Metal goes through DXIL.

**id Tech 7** (SIGGRAPH 2020, Doom Eternal). Vulkan only on PC, fully forward, few uber-shader
variations, "completely bindless", a global texture descriptor list maintained during streaming,
"only 3 different indexable layouts in the game", geometry merged into indirect draws, and no main
or render thread ("all jobs with one worker thread per core"). Secondary sources put the pipeline
count under 500. The lowest-overhead AAA path came from content discipline, not RHI cleverness.

**Our Machinery.** A C plugin API whose only backend was Vulkan. Two command buffer kinds: resource
commands that return handles immediately and draw commands with 64-bit sort keys merged across
threads before translation. Resource binders map one-to-one to descriptor sets, with explicit
register allocation and a push to "think about grouping resources by update frequency". PSOs are
created at the last minute from a hash of render pass, shader and state blocks through an
immutable global map and per-thread arrays, with "no locks or atomics". The render graph above
handles barriers, multi-GPU copies, transients and culling. Sort-key command buffers decouple
systems but push all state-tracking cost into translation.

**Bevy and wgpu.** wgpu layers an idiomatic API over wgpu-core (IDs, validation, state tracking)
over wgpu-hal (unsafe, no tracking, explicit barriers, static dispatch). wgpu issue #1413 found
that "the Vulkan driver is taking less than half of that time. The rest is spent on barriers and
tracking", and #5766 measured 140 ms for 1,000 bindless dispatches against 19.5 ms for 10,000
non-bindless ones, from speculative barriers. Bindless remains experimental. On Metal, buffer
binding arrays are missing and the 31-slot argument buffer limit forced Bevy into per-backend
binding array sizes (64 per group on Metal, 2048 elsewhere). Bevy 0.19 replaced its RenderGraph
with ECS schedules.

**LunaSDK** (JX-Master). C++ COM-style interfaces returning `R<Ref<T>>`, built with xmake. D3D12,
Vulkan and metal-cpp Metal on the Metal 3 model (argument buffers written with `gpuAddress()` and
`gpuResourceID()`, `useResources` per pass, fences across encoders). Vulkan-style sets with a
variable-count last binding for bindless. Passes are explicit scopes, and "all pipeline states must
be valid only in a pass scope". Barriers take before and after states, and `before = automatic`
uses tracked state that "is valid even between multiple command buffers and multiple command
queues", provided submission order equals execution order.

**NVIDIA Falcor and slang-rhi.** Falcor 9.0 still wraps Slang's `gfx` layer; slang-rhi is gfx's
successor, used by SlangPy, so the move is in the Falcor lineage rather than in Falcor itself.
slang-rhi binds through a tree of `IShaderObject`s mirroring Slang's parameter layout:
`bindPipeline` returns the root object to fill, and each backend lays the tree out natively (root
signature and tables, descriptor sets, argument buffers with tier 2). It records into a host-side
command list built at `finish()`, to "encode commands in parallel, even if backend doesn't support
multi-threading" and to create specialized pipelines in parallel. Barriers are tracked
automatically. Its Metal backend uses the Metal 3 model.

**kajiya** (Embark, archived). Rust over ash, Vulkan only. The render graph is the real API: handles
are versioned on write, passes declare `vk_sync` access types and receive typed views, and barriers
are derived from them. The README admits "Vulkan API usage is extremely basic … barriers aren't
optimal".

**Flax Engine.** C++ `GPUDevice` and `GPUContext` with DX11, DX12, Vulkan (MoltenVK on macOS) and
WebGPU backends. The context is D3D11-shaped (`BindSR`, `BindUA`, `BindCB`, `SetState`). Recent
code retrofits explicit control as scoped helpers: `GPUPass`, `GPUMemoryPass` and `GPUComputePass`
issue transitions early, batch copy barriers and overlap UAV writes, over a `GPUResourceAccess`
mask.

**ezEngine.** Its renderer foundation now has bind groups, bind group layouts, pipeline layouts and
pipeline objects, plus explicit batched barriers that "must be called outside of rendering and
compute scopes", where each texture "must currently be in the barrier's m_StateBefore state",
checked in debug builds. Backends are DX11 and Vulkan.

**Lumix Engine.** Free functions in `gpu::` with pointer handles and 32-bit `BindlessHandle`s safe
to fetch from any job, D3D12 and Vulkan backends, a single global root signature (six root CBVs,
unbounded SRV and UAV tables, a sampler table), lazy PSOs hashed from program and framebuffer
state, and `DrawStream` command recording in jobs replayed on the render thread.

**The Forge.** A C API (`addPipeline`, `cmdBindDescriptorSet`, `cmdResourceBarrier`) whose
descriptor sets are grouped by update frequency, with one renderer-wide root signature on D3D12 and
Metal buffers 0 to 3 reserved for one argument buffer per frequency. The header keeps a
non-argument-buffer path alive because "there are argument buffers bugs in some iOS Metal drivers
which causes shader compiler crashes or incorrect shader generation".

**sokol-gfx.** A C header with Metal, D3D11, GL, WebGPU and (since December 2025) an experimental
Vulkan 1.3 backend that requires `EXT_descriptor_buffer`, called "an absolutely perfect match" for
its bind call. Barriers are lazy and tracked per resource, and the author calls them the "biggest
hassle", calls the Vulkan descriptor model "an overengineered boondoggle" and the WebGPU bind group
model "essentially the Vulkan 1.0 descriptor model". The 2025 resource-view update removed
pre-baked pass attachment objects.

**Others.** Cocos Creator's GFX layer is Vulkan-shaped (render passes, subpasses, descriptor sets,
explicit `pipelineBarrier`) over Metal, Vulkan, GLES and WebGPU, with a validator decorator and an
agent decorator that moves the device onto a render thread. Spartan Engine is Vulkan first with a
partial D3D12 path and a `prepare_*` helper style for transitions. Kohi is C with a
function-pointer backend loaded as a Vulkan-only plugin. RavEngine's RGL is a Metal-like API over
Vulkan, D3D12 and Metal with automatic barriers on the explicit backends. Snap's SnapRHI targets
Metal, Vulkan and GLES with `final` backends for devirtualization and optional unretained
resources. Hazel's current engine is private. No primary Ubisoft (Anvil, Snowdrop) or Activision
RHI or render graph talk was found; the closest Ubisoft-era source is the SIGGRAPH 2015
GPU-driven pipelines talk.

## 2. Sources (accessed 2026-10-11)

Unreal Engine:
- <https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RHI/FDynamicRHI>
- <https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RHI/FRHICommandList>
- <https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RHI/FRHIComputeCommandList>
- <https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RHI/FRHICommandListBase>
- <https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RHI/FRHIResource>
- <https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RHI/FRHITransitionInfo>
- <https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RHI/FRHIBatchedShaderParameters>
- <https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RHI/ERHIBindlessConfiguration>
- <https://dev.epicgames.com/documentation/unreal-engine/API/Runtime/RHI/IRHITransientResourceAllocator>
- <https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine>
- <https://dev.epicgames.com/documentation/en-us/unreal-engine/parallel-rendering-overview-for-unreal-engine>
- <https://dev.epicgames.com/documentation/en-us/unreal-engine/pso-precaching-for-unreal-engine>
- <https://dev.epicgames.com/documentation/en-us/unreal-engine/optimizing-rendering-with-pso-caches-in-unreal-engine>
- <https://dev.epicgames.com/documentation/unreal-engine/unreal-engine-5-5-release-notes>
- <https://dev.epicgames.com/documentation/en-us/unreal-engine/macos-development-requirements-for-unreal-engine>
- <https://www.unrealengine.com/en-US/tech-blog/unreal-engine-5-2-brings-native-support-for-apple-silicon-and-other-developments-for-macos>
- <https://www.unrealengine.com/tech-blog/bringing-unreal-engine-on-macos-up-to-feature-parity-with-windowsprogress-report>
- <https://issues.unrealengine.com/issue/UE-240793>, <https://issues.unrealengine.com/issue/UE-302299>
- <https://tomlooman.com/unreal-engine-psocaching/>

Godot:
- <https://github.com/godotengine/godot/blob/master/servers/rendering/rendering_device_driver.h>
- <https://github.com/godotengine/godot/blob/master/servers/rendering/rendering_device.h>
- <https://github.com/godotengine/godot/blob/master/servers/rendering/rendering_device_graph.h>
- <https://github.com/godotengine/godot/tree/master/drivers/metal>
- <https://github.com/godotengine/godot/pull/88199>, <https://github.com/godotengine/godot/pull/111976>,
  <https://github.com/godotengine/godot/pull/114484>
- <https://godotengine.org/article/rendering-priorities-january-2024/>
- <https://godotengine.org/article/rendering-acyclic-graph/>
- <https://godotengine.org/article/d3d12-adventures-in-shaderland/>
- <https://dev.vulkan.org/user/pages/09.events/vulkanised-2024/vulkanised-2024-clay-john-godot.pdf>

SakuraEngine, Wicked, Filament, O3DE, Granite:
- <https://github.com/SakuraEngine/SakuraEngine/blob/engine/engine/modules/engine/graphics/include/SkrGraphics/api.h>
- <https://github.com/SakuraEngine/SakuraEngine/blob/engine/engine/modules/engine/graphics/include/SkrGraphics/README.md>
- <https://github.com/SakuraEngine/SakuraEngine/blob/engine/engine/modules/engine/graphics/src/README.md>
- <https://github.com/turanszkij/WickedEngine/blob/master/WickedEngine/wiGraphicsDevice.h>
- <https://wickedengine.net/category/devblog/2021/05/06/graphics-api-abstraction/index.html>
- <https://wickedengine.net/category/devblog/2021/04/06/bindless-descriptors/index.html>
- <https://www.patreon.com/wickedengine/posts/mac-os-version-147081237>
- <https://github.com/google/filament/blob/main/filament/backend/include/private/backend/DriverAPI.inc>
- <https://github.com/google/filament/blob/main/filament/backend/include/private/backend/CommandStream.h>
- <https://github.com/google/filament/blob/main/filament/backend/include/backend/PipelineState.h>
- <https://github.com/o3de/o3de/tree/development/Gems/Atom/RHI/Code/Include/Atom/RHI>
- <https://dev.vulkan.org/user/pages/09.events/vulkanised-2024/vulkanised-2024-akio-gaule-o3de.pdf>
- <https://o3de.org/exploring-the-power-of-vulkan-in-o3de-a-journey-into-modern-graphics/>
- <https://github.com/Themaister/Granite>
- <https://themaister.net/blog/2019/04/14/a-tour-of-granites-vulkan-backend-part-1/> (series parts 1
  to 6)
- <https://themaister.net/blog/2017/08/15/render-graphs-and-vulkan-a-deep-dive/>

Other engines and talks:
- <https://github.com/Unity-Technologies/NativeRenderingPlugin/tree/master/PluginSource/source/Unity>
- <https://docs.unity3d.com/ScriptReference/GraphicsJobMode.html>
- <https://docs.unity3d.com/6000.0/Documentation/Manual/urp/render-graph-introduction.html>
- <https://docs.unity3d.com/Manual/shader-pso-trace.html>
- <https://www.gdcvault.com/play/1024612>
- <https://www.evolvebenchmark.com/blog-posts/bindless-rendering-setup>
- <https://github.com/Traverse-Research/saxaboom>
- <https://advances.realtimerendering.com/s2020/RenderingDoomEternal.pdf>
- <https://ruby0x1.github.io/machinery_blog_archive/post/a-modern-rendering-architecture/index.html>
- <https://ruby0x1.github.io/machinery_blog_archive/post/efficient-binding-of-shader-resources/index.html>
- <https://ruby0x1.github.io/machinery_blog_archive/post/vulkan-pipelines-and-render-states/index.html>
- <https://github.com/gfx-rs/wgpu/blob/trunk/wgpu-hal/README.md>
- <https://github.com/gfx-rs/wgpu/issues/1413>, <https://github.com/gfx-rs/wgpu/issues/5766>
- <https://bevy.org/news/bevy-0-19/>, <https://github.com/bevyengine/bevy/pull/17898>
- <https://github.com/JX-Master/LunaSDK>
- <https://github.com/shader-slang/slang-rhi>
- <https://github.com/NVIDIAGameWorks/Falcor/blob/master/Source/Falcor/Core/API/Device.h>
- <https://github.com/EmbarkStudios/kajiya>
- <https://github.com/FlaxEngine/FlaxEngine/blob/master/Source/Engine/Graphics/GPUPass.h>
- <https://github.com/ezEngine/ezEngine/blob/dev/Code/Engine/RendererFoundation/CommandEncoder/CommandEncoder.h>
- <https://github.com/nem0/LumixEngine/blob/master/src/renderer/gpu/gpu.h>
- <https://github.com/ConfettiFX/The-Forge/blob/master/Common_3/Graphics/Interfaces/IGraphics.h>
- <https://floooh.github.io/2025/12/01/sokol-vulkan-backend-1.html>
- <https://github.com/cocos/cocos-engine/tree/v3.8.9/native/cocos/renderer>
- <https://github.com/RavEngine/RGL>, <https://github.com/Snapchat/SnapRHI>
- <https://www.sebastianaaltonen.com/blog/no-graphics-api>
