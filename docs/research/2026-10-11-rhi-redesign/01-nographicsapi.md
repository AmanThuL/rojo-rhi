# No Graphics API: surface, cross-API mapping, prior evidence and recommendations

**Status**: Proposed

Research input for the top-down RHI redesign targeting Metal 4 and D3D12, kept friendly to a later
Vulkan backend. It analyses Sebastian Aaltonen's *No Graphics API* proposal and its reference
implementation, maps every concept to the three APIs, re-reads the address-first prototype that
Luminex measured before RojoRHI's extraction, and recommends what to adopt. No decision is made
here; ADR 0006 remains the accepted per-frame data decision.

Sources: the blog (<https://www.sebastianaaltonen.com/blog/no-graphics-api>, 2025-12-16), the
SIGGRAPH 2026 Moving Mobile Graphics slides (34 pages, cited as "slide N"), the extended talk
(<https://www.youtube.com/watch?v=aQv9pUl9PBM>, cited by timestamp), the reference library
<https://github.com/sebbbi/NoGraphicsAPI> (MIT; read at its 2026-10-06 tip, headers
`include/NoGraphicsAPI/NoGraphicsAPI.hpp` and `shader.slang`, backends `src/NoGraphicsAPI.cpp`
and `src/NoGraphicsAPIMetal.mm`, docs under `docs/`), and Luminex's M5.1 record (design spec,
execution-model evidence, ADR 0010 "The RHI adopts an address-first per-frame data path; the
object model stays", and the M5.2 migration record). "NGA" below means the reference library;
"the blog" means the proposal.

## 1. The API surface

The blog's prototype is a 150-line sketch (`gpuMalloc`, `gpuSetActiveTextureHeapPtr`,
`gpuBarrier`, `gpuSignalAfter`/`gpuWaitBefore`, `gpuCreateBlendState`, multi-draw with per-draw
root strides). The slides add `gpuTempMalloc(cb, bytes, align)`, "similar to vkCmdPushDataEXT and
Metal setBytes" (slide 13). NGA, released 2026-09-04 and given a native Metal 4 backend on
2026-09-25, is the shipped interpretation; its `namespace gpu` surface is complete below.

| Category | NGA signatures (verbatim, defaults elided) | Notes |
|---|---|---|
| Device | `DeviceInit create_device(const DeviceDesc&)`; `destroy_device`; `const DeviceCaps& get_device_caps(const Device*)`; `bool supports_texture_format(const Device*, Format, TextureUsage)`; `set_error_callback(ErrorCallback)` | `DeviceDesc` carries window (HWND or `CAMetalLayer*`), swapchain format/colour space, queue counts per family, `timestamp_query_count`. Creation reports `Error`; frame operations return nothing and assert. |
| Memory | `GpuHeap create_gpu_heap(Device*, uint64 byte_count, MemoryType = cpu_visible)`; `destroy_gpu_heap(const GpuHeap&)`; `GpuRange gpu_range(GpuCpuRange<T>)` | `MemoryType{cpu_visible, gpu_only, readback}`. `GpuCpuRange<T>{T* cpu; T* gpu; uint64 size}`, `GpuRange{void* gpu; uint64 size}`. Raw blocks; suballocation is the caller's (utility `BumpAllocator`, `HeapAllocator`). Pointers 16-byte aligned. |
| Buffers as pointers | none: there is no buffer type | Typed `T*` fields in shared C++/Slang structs; `GpuRange` for every command that needs address plus length. |
| Textures | `TextureHeap create_texture_heap(Device*, uint64)`; `SizeAlign get_texture_size_align(Device*, const TextureDesc&)`; `Texture* create_texture(CommandBuffer*, const TextureDesc&, const TextureHeap&, uint64 offset)`; `activate_texture_alias(CommandBuffer*, Texture*)`; `RenderView* create_render_view(Texture*, const RenderViewDesc&)` | Textures stay opaque objects (rasteriser and copy engines are not bindless). Creation records initialisation into a command buffer. `aliasable` placements need activation per period of use. |
| Descriptor heaps | `create_texture_descriptor_heap(Device*, uint32 capacity)`; `write_texture_descriptor(heap, index, const Texture*, TextureDescriptorType, const TextureDescriptorDesc&)`; `copy_texture_descriptors(src, si, dst, di, count)`; `set_texture_descriptor_heap(CommandBuffer*, heap)` | Application-chosen 32-bit slots; descriptor bytes are private (the blog's raw `GpuTextureDescriptor{uint64[4]}` was not kept). Views select format, aspect, mip and layer ranges. |
| Samplers | `create_sampler_descriptor_heap`; `write_sampler_descriptor(heap, index, const SamplerDesc&)`; `copy_sampler_descriptors`; `set_sampler_descriptor_heap` | Separate index space, not the blog's embedded samplers. No sampler object. |
| Root data | the `const void* root` first data argument of every draw and dispatch | One 16-byte-aligned GPU address per command, shared by all graphics stages, caller-owned, never copied by the API; GPU work may write its contents. No size limit. |
| Pipelines and state | `create_graphics_pso(Device*, const GraphicsPSODesc&)`; `create_mesh_pso`; `create_compute_pso(Device*, const ShaderStage&)`; `bind_pso`; `set_viewport`; `set_scissor`; `set_depth_stencil(CommandBuffer*, const DepthStencilState&)` | PSOs hold shaders (SPIR-V or metallib plus entry and threadgroup size), colour formats with blend, depth/stencil formats, cull and depth bias. No binding or vertex layout. Viewport, scissor and depth/stencil are command state reset by each pass. |
| Recording | `create_command_pool(Device*, uint32 queue_index)`; `reset_command_pool`; `begin_commands(CommandPool*)`; `end_commands`; `begin_render_pass(CommandBuffer*, const RenderingDesc&, RenderingFlags)`; `end_render_pass` | One-shot buffers from externally synchronised pools; suspend/resume splits a pass across buffers. No implicit barrier at pass boundaries. |
| Draw, dispatch, copy | `draw(cb, root, vertex_count, ...)`; `draw_indexed(cb, root, GpuRange indices, IndexType, ...)`; `draw_indirect(cb, root, GpuRange arguments, draw_count, stride)`; `draw_indexed_indirect`; `dispatch(cb, root, uint32x3)`; `dispatch_indirect(cb, root, GpuRange)`; `draw_meshlets`; `draw_meshlets_indirect`; `copy_memory(cb, GpuRange, GpuRange)`; `copy_memory_to_texture`; `copy_texture_to_memory` | GPU-sourced draw counts, root arrays and per-draw root strides are not exposed. |
| Barriers and sync | `barrier(cb, Stage before, Access before_access, Stage after, Access after_access)`; `create_timeline_semaphore`; `timeline_completed_value`; `wait_timeline(TimelinePoint)`; `wait_idle` | `Stage` is a 12-bit mask "ordered by Vulkan's logical execution order"; `Access` adds `indirect_read`, `descriptor_read`. The blog's split barriers on memory counters are absent. |
| Queues and presentation | `submit(Device*, const SubmitDesc&, uint32 queue_index)`; `SwapchainFrame acquire(CommandBuffer*)`; `submit_and_present(Device*, const SubmitDesc&)`; `set_swapchain_format`; `write_timestamp`; `read_timestamps(CommandPool*)` | Flat queue indices (general, compute, copy). `SubmitDesc{commands, waits, completion}`; completion is mandatory. |
| Shader ABI | `GPU_ROOT(Type, name[, alignment])`; `T gpu_texture<T>(uint index)`; `gpu_sampler<T>(uint)`; `gpu_nonuniform_index`; `GPU_ADDRESS(value)` | Vulkan: root is a UBO at binding 0 mapped from the pushed address; heaps through `SPV_EXT_descriptor_heap`. Metal: `StructuredBuffer<T>` at buffer 0 (keeps C layout; Slang's Metal `ConstantBuffer` changes it), texture-pool base at buffer 1, sampler IDs at buffer 2. Row-major, C layout. |

The blog-to-NGA deltas matter more than the sketch. NGA dropped raw descriptor bytes, embedded
samplers, the separate blend-state object, split barriers on memory values, specialisation
structs, the single shared-virtual-memory pointer and GPU-sourced multi-draw roots, each because
Vulkan or Metal could not express it (`docs/no-graphics-api-comparison.md`). It also dropped an
early `vkCmdPushDataEXT`-copied 256-byte root and command-pool root arenas in favour of
caller-owned root pointers.

## 2. Mapping to Metal 4, D3D12 and Vulkan

Classification: **native** (one native call or none); **cheap** (bounded constant overhead, no
hash map, no extra PSO, no extra GPU indirection); **costly** (hash lookups, PSO permutations,
descriptor copies, per-resource state tracking or extra GPU indirection); **impossible**.

| Concept | Metal 4 | D3D12 (Agility SDK, SM 6.6+) | Vulkan (extensions NGA requires) |
|---|---|---|---|
| Mapped heap `{cpu, gpu}` | native: `MTLHeap` plus one `MTLBuffer` spanning it (`contents`, `gpuAddress`); must also join a residency set | native: heap plus placed buffer, `Map`, `GetGPUVirtualAddress`; `cpu_visible` is `GPU_UPLOAD` (OPTIONS16, ReBAR or UMA) or plain `UPLOAD` | native: memory plus `VkBuffer` with buffer device address; device-local host-visible needs ReBAR |
| Nested typed pointers in shaders | native: Slang emits `device T*` | **costly**: HLSL/DXIL have no pointers; emulate as heap index plus offset over `ByteAddressBuffer` (see below) | native: BDA and `SPV_KHR_physical_storage_buffer` |
| Root pointer per draw | cheap: `MTL4ArgumentTable setAddress:atIndex:0` before each draw (state, not a draw parameter) | cheap: one shared root signature with one root CBV; `Set*RootConstantBufferView` per draw; 256-byte address alignment | native: `vkCmdPushDataEXT` of 8 bytes, `VK_DESCRIPTOR_MAPPING_SOURCE_PUSH_ADDRESS_EXT` |
| GPU-written root contents | native (memory plus barrier) | native | native (UBO read access) |
| App-indexed texture heap | cheap: `MTLTextureViewPool` slots; shader forms a handle from `baseResourceID + index` by reinterpret cast | native: `ResourceDescriptorHeap[i]`; writes via `Create*View` into the shader-visible heap | native: `VK_EXT_descriptor_heap`; descriptor indexing as fallback |
| Descriptor copy within or between heaps | native: `copyResourceViewsFromPool` | cheap: copy sources must be non-shader-visible, so a CPU shadow heap duplicates every write | native |
| Raw descriptor bytes, GPU-written descriptors | impossible | impossible | native |
| Sampler heap | cheap: buffer of 8-byte sampler resource IDs | native: sampler heap (2,048 limit) | native |
| No binding layout in pipelines | native: fixed three-slot argument table | cheap: one global root signature for every PSO | native: null layout, `DESCRIPTOR_HEAP_BIT_EXT` |
| Placed textures, `size_align` | native: `heapTextureSizeAndAlign`, placement heap | native: `GetResourceAllocationInfo`, `CreatePlacedResource` | native, one fixed memory type |
| Texture aliasing activation | native: barrier with device visibility | native: layout `UNDEFINED` plus discard | native |
| Threadgroup size | cheap: CPU must supply it | native: `[numthreads]` | native: SPIR-V |
| Dynamic viewport, scissor | native | native | native |
| Depth/stencil as command state | native: `MTLDepthStencilState` (NGA caches per pool by linear search) | **costly**: depth enable, write, compare and stencil ops are PSO state; only stencil reference, depth bounds and (with dynamic depth bias support) bias are dynamic, so PSO variants keyed by state | native: extended dynamic state |
| Separate blend state (blog only) | impossible | costly: PSO variants | native: dynamic state 3 |
| Index buffer by address range | native: `drawIndexedPrimitives` takes address and length | native: `D3D12_INDEX_BUFFER_VIEW` holds a VA | native: `vkCmdBindIndexBuffer3KHR` |
| Indirect arguments by address | native, but one call per draw in `draw_count` | cheap: `ExecuteIndirect` takes resource plus offset and a command signature | native: `vkCmd*Indirect2KHR` |
| Copies by address | cheap: copy commands need an `MTLBuffer`; NGA searches at most 64 live heaps | cheap: `CopyBufferRegion` needs a resource; same lookup | native: `vkCmdCopyMemoryKHR` |
| Resource-free stage/access barrier | native: producer barrier `barrierAfterStages:beforeQueueStages:` | buffers native (`D3D12_GLOBAL_BARRIER`); **textures costly**: "Global barriers have no effect on texture layout" | native: global `VkMemoryBarrier2`, `GENERAL` layout |
| Split barrier on a memory value (blog only) | impossible | impossible | impossible as specified |
| Timelines, multi-queue, one-shot buffers | native: `MTLSharedEvent`, `MTL4CommandAllocator` | native: fences, allocators | native |
| Render pass suspend/resume | native: `MTL4RenderEncoderOptionSuspending` | native: render-pass suspend/resume flags | native: dynamic rendering flags |
| Timestamps read straight to CPU | native: counter heaps | cheap: needs `ResolveQueryData` into a readback buffer | native: `vkGetQueryPoolResults` |
| Specialisation struct with pointers (blog only) | impossible: function constants are indexed scalars | impossible: no specialisation constants | partial: scalars only |
| Residency (absent from the model) | required: residency sets | optional: resident on creation; `MakeResident`/`Evict` for budgets | not needed |

**D3D12 pointers.** HLSL and DXIL have no pointer type, and Slang's user guide lists pointer
support only for SPIR-V, C++ and CUDA targets (Metal works in practice, verified by NGA and by
M5.1). Aaltonen's own answer: "DirectX 12 is impossible, since DX12 doesn't have pointers in
shaders" (<https://github.com/sebbbi/NoGraphicsAPI/issues/17>, 2026-09-16). The root level maps
well, because a root CBV or root SRV takes a raw GPU VA. Anything a shader follows *from* the root
cannot use a VA. The workable lowering gives each heap a raw SRV/UAV in the shader-visible heap and
encodes a pointer as `(viewIndex << 32) | offset`. Allocation produces that value once, so later
use needs no lookup, and byte-offset arithmetic stays valid inside one view. Shaders read it
through `ResourceDescriptorHeap[index].Load<T>(offset)`. The cost is real. Each distinct base
fetches a descriptor. Raw loads promise only 4-byte alignment, which can split wide loads (the
blog's own NVIDIA complaint). Divergent bases need `NonUniformResourceIndex`, which on AMD means
scalarisation loops. Very large heaps may need several views. Shared C++ structs then carry an
encoded value rather than the VA, so the shared header abstracts the pointer type: a Slang
`GpuPtr<T>` wrapper that lowers to `T*` on Metal and SPIR-V and to the heap load on DXIL. Shaders
cannot use raw `T*` syntax portably. Root CBVs also need 256-byte aligned addresses, not NGA's 16,
an alignment RojoRHI's `kFrameDataAlignment` already assumes. **D3D12 textures and barriers.**
Enhanced barriers give a true resource-free `D3D12_GLOBAL_BARRIER`, but render-target,
depth-stencil and UAV textures still change layout before they can be sampled. The only
layout-free textures are simultaneous-access ones, which cannot be depth-stencil written and are
"generally much slower". A D3D12 backend for a resource-free barrier API therefore has to track
per-texture layouts, which is exactly the bookkeeping the model removes.

**Metal 4 semantics.** Texture handles are 64-bit `MTLResourceID`s. Indexing a pool by
`baseResourceID + index` relies on an undocumented representation (NGA's `gpu_texture_from_id`
reinterpret cast, from the `sebbbi/msl_heap` experiment), and Apple was asked for language support
(talk 1:33:41). A texture also cannot be a bare pointee: `device texture2d<float>*` is rejected,
so a table of handles needs one-member wrapper structs (M5.1 evidence 5.7, 5.9). Argument tables
are encoder state: a root costs one `setAddress` per draw, and NGA binds the table to every render
stage, so "per-stage roots" do not exist on Metal. Every allocation must sit in a committed
residency set attached to the queue. NGA keeps one device-wide set, committed under a lock at heap
creation and destruction, plus the layer's own set. Barriers come in two scopes that do not nest:
queue-scope orders earlier encoders and encoder-scope orders earlier commands in the same encoder,
and M5.1 had to emit both when a producer straddled them. NGA sidesteps this by encoding each
barrier as a producer barrier in a compute encoder it then closes. That costs encoder churn and
forbids barriers inside render passes. NGA also maps `cpu_visible` heaps to
`MTLResourceCPUCacheModeWriteCombined`, which M5.2 measured at 42.6 ns per scattered 64-byte write
on M3 Max (a 32% regression until removed).

## 3. What M5.1 measured, and why

M5.1 (2026-08-12, M3 Max, macOS 26.5.2, 12 paired fresh-process repetitions, frozen thresholds)
compared Luminex's object-shaped RHI with `lmx::experimental::noapi`, a Metal 4 prototype written
from the blog. NGA did not exist yet. The prototype had `allocate` returning `{cpu, gpu}`,
`LinearAllocator`, `pushRoot` copying a block into a frame allocator, `draw(vertexRoot, pixelRoot,
...)`, one bindless table of 8-byte resource IDs with generation-checked handles, stage-pair
barriers with `Hazard` flags, `signalAfter`/`waitBefore`, one residency set and `FrameRing`. ADR
0010 chose a **partial reshape**, which RojoRHI restates as ADR 0006 (`bindFrameData`).

| Result (scored) | Value | Cause | Intrinsic to the model, or artefact? |
|---|---|---|---|
| CPU encoding, 13-pass graph | 9.284 to 0.352 ms/frame (−96.2%) | Incumbent created 1,025 buffers per frame: per-draw uniforms past a fixed 256 KiB ring, no buffer write path, and an O(n) capture-registry scan per `createBuffer` (M5.2 attributed 12% to 45% of comparable dynamic baselines to it) | Mostly an incumbent artefact. The durable part, ring-suballocated per-draw data plus an address bind, is not unique to the model; M5.2 kept it through `bindFrameData` (94.6% and 97.7% faster on dynamic workloads, static workloads unchanged) |
| Allocations, 272 frames | 279,832 to 9 | Same cliff | Same |
| Binding calls, graph | 10,277 to 4,106 (−60%) | Bindless: 9,243 texture and buffer binds became 3,081 `setAddress` | Intrinsic win of bindless and address-first binding |
| Barriers, graph | 16 to 9 (−43.8%) | One stage barrier covers every resource between a pass pair | Intrinsic win |
| S-BIND CPU, 1,024 / 4,096 draws | +8.5% / +10.8% (immaterial) | Two static roots re-pushed (`memcpy` plus two `setAddress`) per draw, a binary search per indexed draw for the index length, write-combined root memory | Artefact. Static roots could be persistent allocations. NGA passes `GpuRange` (no lookup) and one root. The WC penalty was proven in M5.2; its M5.1 share is inferred |
| S-BIND calls and bytes | +100%; 0 to 64–256 KiB per frame | Same re-push | Artefact |
| API surface | +58% concepts, +33% operations, +108% arguments | Kept Texture, Sampler, Pipeline, depth-stencil state, Semaphore, ResidencySet and BindlessTable, then added allocator, suballocation, frame ring, handles, capabilities and hazard enums; the incumbent had no descriptor-set objects to remove | Partly intrinsic: memory policy moves to the caller (NGA moves it to a separate utility library). Partly scale: the incumbent was already argument-table shaped |
| Isolated hazard barriers | 2 to 3–5 primitives per case (+50% to +150%) | Stage barriers cannot scope to one resource; Metal's two non-nesting scopes doubled some emissions; deferred per-encoder-class flushing | Over-synchronisation is intrinsic. The primitive count is a Metal-lowering artefact. GPU cost was not measured |
| Hazard coverage | 24/24 vs 19/24 | Incumbent lacked per-mip attachments | Incumbent gap |
| Misuse M5 | clean assert vs SIGSEGV | Generation-checked slots | Prototype addition, not the model |
| Split barrier wait | predicate checked on the CPU at record time; 32-bit atomics only | No wait-on-memory command; no 64-bit device atomics | Intrinsic impossibility (NGA dropped it) |
| Specialisation struct | refused | Metal function constants are scalars | Intrinsic (NGA dropped it) |
| Requested bytes (unscored) | +75.7% | Up-front pooled allocations | Policy, not the model |

The prototype won where the incumbent paid for objects per frame, and where bindless and stage
barriers collapse many small operations into one. It lost where it re-pushed static data or
looked up addresses on hot paths, both artefacts NGA's later design avoids. It also lost on API
surface, because address-first moves allocation and lifetime policy to the caller rather than
deleting it. Its own D3D12 paper column marked shader pointers, the GPU-written table and the
memory-wait split barrier as emulated, unavailable or unknown, which is why ADR 0010 kept the object
model outside per-frame data. One correction to that column: D3D12 descriptors *can* be written
straight into a shader-visible heap's CPU handle; only copies need a non-visible source.

## 4. Critiques and community feedback

- **D3D12 is the outlier, by the author's own account.** Besides issue 17, the talk (1:27:41 to
  1:29:20) calls D3D12 "the most problematic" API: abstracted descriptor heap, no pointers in HLSL,
  messy root APIs, resource-list barriers. Slide 31's matrix marks D3D12 red on pointers, roots and
  barriers.
- **The baseline is narrow even on Vulkan.** NGA's 2026-09-05 driver snapshot lists Windows RDNA 2,
  Pascal and the checked Intel Arc report as unsupported, and fixed 256 MiB BAR (Turing) as
  capacity-limited. Linux needs Mesa 26.2+ (README).
- **Hacker News** (<https://news.ycombinator.com/item?id=46293062>, 845 points, 183 comments).
  Practitioners agree on the PSO and barrier diagnosis. Raised gaps: GPU virtualisation and
  residency ("the D3D residency APIs"), undefined behaviour that blocks future translation, the
  ReBAR floor, render passes still mattering on tilers, and that the performance gains are
  "deep in 'diminishing returns area'" for a renderer already on the modern subset (flohofwoe).
  A WebGPU contributor called the one-screen claim a "powerpoint-driven-development flex".
- **Peter Cawley** (<https://www.corsix.org/content/thoughts-on-no-graphics-api>): handing
  write-combined pointers to user code creates "lots of potential footguns", the M5.2 pitfall.
  Cross-command-buffer signal/wait can deadlock unless in-order execution is always valid.
  Multi-device and multi-process allocation are missing.
- **Loon GPU** (<https://www.kevin-gibson.com/blog/writing-a-bindless-gpu-abstraction-layer/>), a
  Vulkan 1.3 plus Metal 4 implementation, made the same retreats: opaque texture heaps, blend in
  the PSO, a redesigned split barrier, a sorted-list address-to-buffer lookup, and hand-written MSL
  because Slang's Metal path was immature. Its global "uninitialised images go to `GENERAL` at the
  next submit" step creates a cross-queue sync point.
- **NGA issue 2** (author replies): stateless index-pointer-per-draw is preferred over stateful
  index binding ("previous state leaks"). `VK_EXT_shader_object` was held back pending benchmarks
  because it may cost GPU performance.
- **Ports.** Only Vulkan, Metal and OpenGL interpretations exist (`roeyb1/sgpu`,
  `UnNabbo/no_api`, `LeonardoTemperanza/no_gfx_api`, `theaveasso/jagfx`, language bindings). No
  D3D12 port of the model was found on GitHub or the web as of 2026-10-11.

## 5. Recommendations

NGA is Vulkan-shaped by construction. It is Vulkan 1.4 plus recent extensions, its `Stage` enum
follows Vulkan's execution order, it uses `GENERAL` layouts, and it depends on
`VK_EXT_descriptor_heap`, `vkCmdPushDataEXT` and `VK_KHR_device_address_commands`, the newest
Khronos binding extensions. Its native Metal 4 backend shows the model also fits Metal, at the
price of one undocumented handle trick, residency sets and address lookups for copies. D3D12 is
where it breaks: shader pointers, texture layouts and PSO-baked depth state. ADR 0004 forbids a
Vulkan-shaped interface, so the right target is the intersection that maps natively or cheaply on
Metal 4 *and* D3D12, then checked against Vulkan. That intersection is most of NGA's binding and
memory model, but not its barrier model, nor raw pointers as the universal data reference.

| Verdict | Idea | Reasoning tied to cost |
|---|---|---|
| Adopt | One root address per draw or dispatch, shared by all stages | One state call on every API (`setAddress`, root CBV, push data). Extend ADR 0006 so persistent or static roots bind by address without a re-copy, closing the S-BIND artefact |
| Adopt | App-owned indexed texture and sampler heaps with opaque write and copy operations, 32-bit indices in data | D3D12 and Vulkan native, Metal cheap. Removes per-material binding and descriptor-set hashing. Keep the two index spaces separate (D3D12 requires it) |
| Adopt | Vertex pulling, no vertex layout; index and indirect data as `{address, size}` ranges | Native on all three. Removes vertex-layout PSO permutations. Carry the owning heap in the range so Metal copies and D3D12 `ExecuteIndirect` need no lookup |
| Adopt | Mapped CPU-visible memory as the upload path; placed textures in app heaps; explicit pass load/store without implicit barriers; one-shot pools; timelines; dynamic viewport and scissor | Native everywhere. Use cached shared storage, not write-combined, on Apple silicon |
| Adapt | Pointers in shared data | Keep a portable `GpuPtr<T>` (native on Metal and SPIR-V, view-index plus offset on DXIL) and an opaque 64-bit shader address with offset helpers. Budget D3D12's extra descriptor fetch and raw-load cost; prefer shallow pointer graphs on hot shading paths |
| Adapt | Resource-free barriers | Stage plus access as the primitive (native on Metal, D3D12 global barriers and Vulkan), but name layout-bearing textures (attachments and storage images) so D3D12 can transition them. Metal and Vulkan ignore the list. Let the render graph emit them |
| Adapt | Thinner PSOs | Blend, cull and depth/stencil stay pipeline state, since D3D12 bakes them; stencil reference and, where supported, depth bias are dynamic. This avoids hidden PSO-variant hash maps on D3D12 |
| Adapt | Residency | Internal, heap-granular, one device-wide Metal set committed on heap create and destroy. No public concept; a D3D12 budget hook later |
| Adapt | `gpuTempMalloc` | Already ADR 0006's one-call allocate, copy and bind |
| Reject | Raw or GPU-written descriptor bytes | Impossible on Metal and D3D12 |
| Reject | Split barriers on memory counters | Impossible on all three; NGA dropped them |
| Reject | Specialisation structs with pointers; separate blend state | Impossible on Metal and D3D12, or costly PSO variants |
| Reject | Universal raw pointers with no D3D12 lowering | Would make the D3D12 backend a shader translator |
| Defer | GPU-sourced roots per draw in multi-draw | D3D12 can express it (root CBV in a command signature) and Metal through ICBs, but Vulkan needs device-generated commands; a separate capability |

Expected effect: the CPU per-draw path becomes one address write plus one draw on all targets,
with no descriptor-set hashing and no vertex-layout permutations. The GPU cost lands only on
D3D12's emulated nested pointers. The barrier surface stays resource-aware exactly where D3D12
needs it, which also answers M5.1's isolated-hazard regression.
