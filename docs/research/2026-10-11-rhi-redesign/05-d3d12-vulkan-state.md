# D3D12 and Vulkan as of October 2026: state, hardware floor and No Graphics API mapping

**Status**: Proposed

Research input for the top-down RHI redesign. RojoRHI ships Metal 4 only; D3D12 is a design
target (ADR 0004) and Vulkan is a later candidate. This note records where both APIs stand on
2026-10-11, what hardware and OS each feature needs, what the Steam population can run, and how
the No Graphics API (NGA) model maps onto each. It changes no decision.

Evidence markers: **[V]** read in a primary source (Microsoft, Khronos, vendor, spec repository,
Valve) on 2026-10-11; **[S]** secondary source only (press, forum, community repository);
**[U]** unverified or inferred by this note. Sources are listed at the end with their dates.

## 1. Findings that change the design space

1. **Vulkan now has an NGA-shaped binding model.** `VK_EXT_descriptor_heap` (Vulkan 1.4.340,
   January 2026 [S][s9]) removes descriptor sets, pools and layouts, adds `vkCmdPushDataEXT`, and
   the registry marks `VK_EXT_descriptor_buffer` as deprecated by it [V][k1][k2][k9]. With
   `VK_KHR_device_address_commands` (draw-indirect, index binding, copies and range barriers on raw
   addresses) and `VK_KHR_unified_image_layouts` (`GENERAL` nearly everywhere), Vulkan can express
   pointers + heap indices + one root pointer + resource-free barriers natively [V][k5][k6].
2. **D3D12 has not moved the same way.** It has no GPU pointers in HLSL/DXIL, no CPU-writable or
   GPU-writable descriptor memory, and texture barriers still carry layouts. SM 6.6 heap indexing,
   enhanced barriers, root descriptors by GPU VA and GPU upload heaps get most of the way; the rest
   is emulated in the backend (section 5) [V][d6][d7].
3. **Work graphs are frozen at SM 6.9.** DXIL 1.10 drops node shaders, so SM 6.10 features cannot
   be mixed with work graphs; drivers keep supporting them [V][h2]. The successor in spec form is
   *Work Lists* (per-draw PSO selection from a GPU program table), preview hoped for 2027 [V][d8].
   Cooperative vectors are deprecated in favour of the SM 6.10 `linalg` matrix API [V][d2].
4. **DirectX-on-SPIR-V has not shipped.** SM 7 with SPIR-V interchange was announced in September
   2024 as multi-year; the newest shader model is 6.10 (preview) and no SM 7 date is public
   [V][d9][S][s4]. Plan on DXIL for the D3D12 lifetime of this redesign.
5. **Sebastian Aaltonen shipped `sebbbi/NoGraphicsAPI` (September 2026)** on Metal 4 and Vulkan 1.4
   with descriptor heap + device address commands + mesh shaders + untyped pointers. Its 5 September
   2026 driver snapshot: NVIDIA Turing+ and AMD RDNA 3+ on Windows, RADV/ANV on Mesa 26.2+; not
   Windows RDNA 2, Pascal or the checked Intel Arc. No D3D12 backend; closest prior art [S][n1].
6. **Linux players are reached through Proton.** Steam Linux share was 3.05% in September 2026
   (macOS 1.92%, Windows 95.03%); vkd3d-proton implements enhanced barriers, GPU upload heaps,
   SM 6.6+ and (opt-in since May 2026) a descriptor-heap path [V][v1][v2][S][s1].

## 2. D3D12

### 2.1 Agility SDK timeline (selected)

| Version | Date | Relevant content |
|---|---|---|
| 1.4.9 | 2021-04 | DirectX 12 Ultimate, SM 6.6 (`ResourceDescriptorHeap`, 64-bit atomics, `WaveSize`) |
| 1.608.0 | 2022-11 | Enhanced barriers; independent front/back stencil ref and masks; dynamic depth bias and IB strip cut (PSO flags); triangle fans |
| 1.613.x | 2024-03 | SM 6.8, work graphs, GPU upload heaps, generic programs in state objects, incrementing constant in ExecuteIndirect |
| 1.614 / 1.615 | 2024-05 / 2025-01 | R9G9B9E5 RT/UAV; shader hash bypass |
| 1.616.0 | 2025-05 | Opacity micromaps, tiled resources tier 4 |
| 1.618.x | 2025-09..12 | Advanced Shader Delivery (state object database, SODB), app-specific driver state, tight placed-resource alignment, recreate-at-GPUVA |
| 1.619.0 | 2026-02-26 | SM 6.9 retail (long vectors, SER, OMM, 16/64-bit ops required); byte-offset buffer views + `Try*` view creation returning `HRESULT`; trim notifications; CPU-timeline query resolve; device-specific 1D dispatch limit |
| 1.619.6 | 2026-09-14 | Latest retail patch (SODB tooling fixes) |
| 1.719-preview | 2026-02-26 | Fence barriers (signal/wait fences inside command lists), `ID3D12Extension` IHV extension mechanism, VPblit 3D LUT |
| 1.720-preview | 2026-04-27 | SM 6.10 preview: `linalg::Matrix`, group wave index, variable groupshared size, `TriangleObjectPositions`; batched async clears/fills/copies |
| 1.721.3-preview | 2026-07-30 | Partial graphics programs, GUID texture layouts, UAVs on depth, LinAlg `VectorAccumulate` |

All rows [V] [d1][d2][d3][d4]. The downloads page states no OS floor; the 2021 launch post said
Windows 10 1909+ [V][d10]; whether 1.619 still loads on Windows 10 was not checked [U].

### 2.2 Barriers

- Enhanced barriers split sync scope, access and texture layout. `D3D12_GLOBAL_BARRIER` takes only
  sync and access, so buffer hazards need no resource list [V][d6]. Buffers and simultaneous-access
  textures have no layout. Ordinary textures do: `COMMON` and the queue-specific `*_COMMON` layouts
  allow copy, SRV and (queue-specific) UAV access without barriers, but render target and depth
  access require a texture barrier with `LayoutBefore`/`LayoutAfter` [V][d6].
- Queue-specific layouts (`DIRECT_QUEUE_SHADER_RESOURCE` and similar) avoid decompression when a
  texture stays on one queue type; the spec recommends them for single-queue engines [V][d6].
- Enhanced barriers remain a driver-optional cap (`OPTIONS12.EnhancedBarriersSupported`); the spec
  lists deprecating legacy `ResourceBarrier` as a goal while keeping it functional [V][d6]. Current
  NVIDIA, AMD and Intel drivers report support on maintained hardware [U]; vkd3d-proton implements
  it [V][v1].
- Fence barriers (preview): tier 1 needs 64-bit atomics; tier 2 needs user-mode submission and
  native fences ("hardware scheduling stage 3") [V][d6].

### 2.3 Binding

- SM 6.6 `ResourceDescriptorHeap[i]` / `SamplerDescriptorHeap[i]` need resource binding tier 3 for
  full-heap indexing and root signature flags `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` /
  `SAMPLER_HEAP_DIRECTLY_INDEXED` [V][d11]. One shader-visible view heap (1,000,000+ at tier 3)
  and one sampler heap (2048) are bound at a time [V][d15]; `OPTIONS19` reports per-device heap
  size caps whose shipping values were not checked [S][s5].
- A root signature is still mandatory, but one global signature (root constants + a root CBV by GPU
  VA + the two heap flags) serves every pipeline. Root arguments total 64 DWORDs; a root
  descriptor costs 2 DWORDs and is an unchecked GPU VA [V][d15].
- Descriptors are created with `Create*View` into a CPU handle of the shader-visible heap; there
  is no raw descriptor-byte write and no GPU write. 1.619 adds byte-offset buffer views [V][d7].
- HLSL has no pointer type for DXIL. `vk::BufferPointer` (proposal 0010) is SPIR-V-only; the
  "Loading from Buffer Addresses" request (hlsl-specs issue 17) is still open [V][h1][h3].
  Pointer-style data on D3D12 is a `(heap index, byte offset)` pair read through
  `ByteAddressBuffer.Load<T>` [U: standard practice, not a Microsoft recommendation].

### 2.4 Memory

- Heap types: `DEFAULT`, `UPLOAD` (system memory, write-combined), `READBACK`, `CUSTOM`, and
  `GPU_UPLOAD` (CPU-visible VRAM). `GPU_UPLOAD` needs `OPTIONS16.GPUUploadHeapSupported`, Windows
  11 build 26080+ (24H2) and Resizable BAR on discrete GPUs; UMA parts need no BAR [V][d12][S][s6].
- Tight placed alignment (1.618) shrinks small-buffer alignment; recreate-at-GPUVA (1.618) lets
  capture/replay tools re-create resources at identical GPU VAs, which pointer-heavy apps need for
  PIX-style replay [V][d1].

### 2.5 Pipelines and dynamic state

- PSOs bake shaders, input layout, rasterizer, blend, depth-stencil (test, write, func, stencil
  ops) and formats. Command-list dynamic state: viewport, scissor, blend factor, stencil refs
  (front/back separate since 1.608), depth bounds, and depth bias / strip cut when the PSO opts in
  [V][d5]. There is no dynamic depth-test enable or compare op.
- Caching: `ID3D12PipelineLibrary`, driver caches, and Advanced Shader Delivery (SODB collected by
  the developer, compiled by the store). ASD reaches retail in October 2026 on Windows 11 24H2+:
  AMD RDNA 1-4 now, NVIDIA RTX and Intel Arc B / Core Ultra 2-3 later in the month [V][d13].
- Generic programs (1.613) and `SetProgram` bring state-object linking to graphics; partial
  graphics programs (1.721-preview) pre-compile pre-raster and pixel parts and late-link blend.
  Preview support: RDNA 3/4, Intel Xe2+ and NVIDIA RTX in future drivers [V][d4][d14].

### 2.6 GPU-driven work

- ExecuteIndirect: draw, indexed draw, dispatch, mesh dispatch, rays, plus root constant, root
  descriptor, VB/IB changes per record, count buffer, and (1.613) an incrementing constant for draw
  IDs; it cannot change PSO [V][d8]. SM 6.8 adds `SV_StartVertexLocation`.
- Work graphs (SM 6.8) and mesh nodes (preview 1.715) exist but are frozen at SM 6.9 [V][h2].
  Work Lists spec v0.852 (2026-09-11): GPU program table, per-PSO argument layouts, tier 2
  continuation chains; no implementation yet [V][d8].

### 2.7 Hardware and OS floor

| Capability | Requirement | Hardware with maintained drivers |
|---|---|---|
| SM 6.6 heap indexing | Resource binding tier 3 + SM 6.6 driver | NVIDIA Turing+, AMD RDNA 1+, Intel Xe-LP+ [U] |
| Enhanced barriers | `OPTIONS12` cap | Same set on current drivers [U] |
| DX12 Ultimate (FL 12_2: mesh, DXR 1.1, VRS 2, sampler feedback) | FL 12_2 | NVIDIA RTX 20+ (not GTX 16), AMD RDNA 2+, Intel Arc A+ [S] |
| GPU upload heaps | Win 11 24H2 + ReBAR (dGPU) | Driver-dependent; check cap [V][d12] |
| SM 6.9 long vectors | SM 6.9 | AMD RX 9000, Intel Arc B, all RTX [V][d2] |
| SER that actually reorders | SM 6.9 | RTX 40+, Intel Arc B; RX 9000 accepts but does not reorder [V][d2] |
| Partial programs, UAV of depth | 1.721-preview | RX 7000/9000, Xe2+, RTX (future driver) [V][d4] |

NVIDIA's 580 branch was the last for Maxwell, Pascal and Volta (security fixes only through 2028)
[S][s7]; this note treats those GPUs as outside any new floor.

## 3. Vulkan

### 3.1 Versions and profiles

- Latest spec: 1.4.365 (2026-10-02) [V][k3]. Vulkan 1.4 core includes dynamic rendering local
  read, maintenance 5-6, push descriptors and host image copy (feature optional until Roadmap
  2026), plus dynamic rendering, synchronization2, BDA and timeline semaphores from earlier
  versions [V][k4][k9].
- Roadmap 2024 adds maintenance5, dynamic rendering local read, line rasterization and limits.
  Roadmap 2026 (January 2026) requires Vulkan 1.4 plus robustness2, pipeline binary, fragment
  shading rate, shader clock, compute derivatives, maintenance 7-9, copy memory indirect,
  `VK_KHR_shader_untyped_pointers`, cooperative matrix, present id2/wait2 and swapchain
  maintenance; it does not require descriptor heap, device address commands, unified image layouts
  or mesh shaders [V][k4][k1].

### 3.2 Binding, memory and commands

| Extension | Status (registry, 2026-10) | What it gives an NGA-style RHI |
|---|---|---|
| `VK_EXT_descriptor_heap` | EXT, KHR promotion pending feedback; release task list still open | One resource heap + one sampler heap as plain memory (`vkCmdBindResourceHeapEXT`/`SamplerHeapEXT`), CPU writes via `vkWriteResourceDescriptorsEXT`, no pipeline layout (`VK_PIPELINE_CREATE_2_DESCRIPTOR_HEAP_BIT_EXT`), `vkCmdPushDataEXT` (min 256 B, 512 recommended), embedded samplers (min 2032). Image 64 B / buffer 128 B / sampler 32 B base sizes. Modelled on SM 6.6 heaps [V][k2] |
| `VK_EXT_descriptor_buffer` | Deprecated by descriptor heap | Superseded; do not target [V][k9] |
| `VK_KHR_shader_untyped_pointers` | KHR, in Roadmap 2026 | SPIR-V capability required by the heap shader path [V][k4] |
| `VK_KHR_buffer_device_address` | Core 1.2 | 64-bit pointers in shaders (`PhysicalStorageBuffer`) |
| `VK_KHR_device_address_commands` | KHR, ratified | Index/vertex binding, all indirect draws/dispatches, copies, fill/update and `VkMemoryRangeBarrierKHR` on addresses; buffer objects remain for legacy commands [V][k5] |
| `VK_KHR_unified_image_layouts` | KHR, ratified (2025-06) | `GENERAL` for all uses except init from `UNDEFINED` and present [V][k6] |
| `VK_EXT_device_generated_commands` | EXT | GPU-written commands incl. pipeline/shader switches via indirect execution sets |
| `VK_KHR_maintenance7-11` | KHR (7-9 in Roadmap 2026) | Incremental fixes; 10/11 content not reviewed here [U] |

### 3.3 Pipelines and dynamic state

Dynamic rendering (core 1.3) removes render pass and framebuffer objects; local read (core 1.4)
covers tile-local reads. Extended dynamic state 1/2 are core; EDS3 and `VK_EXT_shader_object` are
optional (gpuinfo report coverage 44% and 24%) [V][k7]. `VK_KHR_pipeline_binary` (Roadmap 2026)
standardizes pipeline caching. Viewport, scissor, depth test/write/compare, stencil ops and cull
mode are all dynamic in core, which is closer to Metal than D3D12 is.

### 3.4 Driver support for the NGA extension set

| Driver | Descriptor heap | Notes |
|---|---|---|
| NVIDIA Windows/Linux | 595 beta, 610+ release; Nsight 2026.2 support | Turing+ per the NGA snapshot; device-lost reports with vkd3d-proton on 610 [V][k8][S][n1][s8] |
| AMD Adrenalin (Windows) | 25.30.17.02 (2026-01) and later | RDNA 3/4 expose it; RDNA 2 does not (26.9.1 snapshot) [S][s11][n1] |
| Mesa RADV / ANV | Default-on in Mesa 26.2 (2026-08-05) | RADV covers Steam Deck (RDNA 2); NVK status not found [V][m1] |
| Intel Windows (Arc) | Not observed | Checked Arc report lacked it [S][n1] |
| KosmicKrisp (Vulkan on Metal) | Not established | Vulkan 1.4 conformant per XDC 2026 talk; MoltenVK not used by NGA [S][s10] |

gpuinfo.org all-time report coverage (skewed low by old and mobile devices): descriptor heap 10%,
unified image layouts 13%, untyped pointers 17%, device address commands 9% [V][k7].

## 4. Market reality (Steam survey, September 2026)

- OS: Windows 95.03% (Windows 11 64-bit 71.34% of all systems), Linux 3.05%, macOS 1.92%. Linux
  peaked near 5% earlier in 2026 and swings month to month [V][v2][S][s1].
- Linux distributions: CachyOS 18.12%, SteamOS Holo 17.37%, Arch 10.67%, Mint 8.74%, Bazzite
  6.97%. Top Linux GPUs are AMD (Radeon 610M 11.18%, Steam Deck 8.60%, custom 0405 7.46%) [V][v3].
- GPUs: DirectX 12 class 92.19%. The top-60 list is dominated by RTX 20-50 and RDNA 2-4; visible
  pre-Turing/pre-RDNA parts (GTX 10, RX 580) total about 4.4%, Turing GTX 16 about 4.7% [V][v4].
  Estimate: roughly 85-90% of Steam GPUs meet SM 6.6 + enhanced barriers and about 75-80% meet
  FL 12_2 [U: derived from the top-60 table only].
- Proton: vkd3d-proton implements enhanced barriers, GPU upload heaps, SM 6.7+, work graph
  emulation, and merged an opt-in descriptor-heap path (`VKD3D_CONFIG=descriptor_heap`) in May
  2026 [V][v1][S][s8]. A D3D12 backend reaches most Linux players at translation cost; a native
  Vulkan backend buys Linux-native tooling, not reach.

## 5. Implications for RojoRHI

Cost: **native** (no emulation), **cheap** (small fixed backend work), **costly** (tracking,
permutations or shader rewriting), **impossible** (cannot be expressed).

| NGA concept | Metal 4 (shipping) | D3D12 (Agility 1.619) | Vulkan 1.4 + heap extensions |
|---|---|---|---|
| GPU pointers in shaders | Native (`gpuAddress`) | Costly: no HLSL pointers; encode `(heap index, offset)` and load via `ByteAddressBuffer`; root CBV/SRV/UAV take raw VA | Native (BDA) |
| Buffers without objects | Native (heap + offsets) | Cheap: one placed buffer per heap, sub-ranges by VA; byte-offset views (1.619) | Native with device address commands; else cheap (one `VkBuffer` per heap) |
| Global texture heap, 32-bit index | Native (`MTLTextureViewPool`, resource IDs) | Native (SM 6.6, tier 3); descriptors via `Create*View`, not raw bytes | Native (descriptor heap) |
| Samplers by index | Native | Native (`SamplerDescriptorHeap`, 2048 cap) | Native (sampler heap, embedded samplers) |
| One root pointer / push data | Native (argument table address) | Native: root CBV by GPU VA + root constants (64-DWORD signature) | Native (`vkCmdPushDataEXT`, >= 256 B) |
| GPU-written roots | Native | Native (root VA from earlier GPU writes; ExecuteIndirect can change root VA per draw) | Native |
| Upload / readback classes | Native (shared storage, UMA) | Cheap: `UPLOAD`/`READBACK`; `GPU_UPLOAD` needs Win 11 24H2 + ReBAR, else fall back | Cheap: host-visible device-local type when ReBAR, else staging |
| Stage-mask barrier, no resources | Native (`MTL4` barriers by stage) | Native for buffers (global barrier); costly for textures (layouts) | Native (`VkMemoryBarrier2`) |
| No image layouts | Native | Costly: backend tracks RT/DS layouts, uses `*_COMMON` / queue-specific read layouts elsewhere | Native with unified image layouts; else cheap (`GENERAL` + attachment transitions) |
| Dynamic depth-stencil state | Native (separate state object) | Costly: baked in PSO; backend permutes and caches PSOs | Native (core dynamic state) |
| Minimal PSO (shaders + raster + blend + formats) | Native | Native shape; caching via pipeline library / ASD; partial programs in preview | Native; pipeline binary for caching |
| Vertex pull, no input layout | Native | Native (empty input layout) | Native |
| Indirect draws with count | Native (ICB) | Native (ExecuteIndirect + count) | Native |
| GPU-chosen pipeline per draw | Native (ICB) | Impossible today (Work Lists, preview 2027 hoped) | Costly/optional (DGC with execution sets) |
| Draw ID / base vertex | Native | Cheap (incrementing constant; SM 6.8 start vertex) | Native |
| Cross-queue / CPU sync | Native (shared events) | Native (fences; fence barriers preview) | Native (timeline semaphores) |

### 5.1 Recommended minimum floor (2026-2027 desktop)

**D3D12 (design target):** Windows 11 x64; Agility SDK 1.619 retail (`D3D12SDKVersion` 619)
redistributed with the app; hardware class FL 12_2 (RTX 20+, RDNA 2+, Arc A+), which matches the
Metal 4 baseline's mesh-shader expectation; SM 6.6 heap indexing and resource binding tier 3
required; enhanced barriers required (no legacy barrier path); independent front/back stencil and
dynamic depth bias as caps; SM 6.8 for start-vertex system values; `GPU_UPLOAD` optional with an
`UPLOAD`-plus-copy fallback. Exclude work graphs (frozen) and cooperative vectors (deprecated)
from the interface; keep SER, long vectors and `linalg` as optional caps.

**Vulkan (later backend, design-for):** Vulkan 1.4 + Roadmap 2026-class device + descriptor heap +
shader untyped pointers + device address commands + mesh shader; unified image layouts optional.
This matches `sebbbi/NoGraphicsAPI`. Today it excludes Windows RDNA 2 and (unconfirmed) Intel
Windows, but covers Steam Deck through RADV, so it is a 2027 target, not a 2026 one [S][n1].

**Interface consequences:** keep a portable pointer ABI that D3D12 can lower to `(index, offset)`
(for example a 64-bit handle the shader library decodes per backend); let render-pass begin/end
own attachment layout transitions so the public barrier stays resource-free; keep depth-stencil
state in the pipeline description (or accept a D3D12 PSO permutation cache); expose GPU-driven
work as indirect draws with count, deferring per-draw pipeline choice until Work Lists/DGC settle.

## 6. Open and unverified items

- D3D12: Windows 10 support of Agility SDK 1.619; enhanced-barrier caps per vendor and
  generation; shipping `OPTIONS19` heap caps; any SM 7 date; any DXIL pointer proposal.
- Vulkan: descriptor heap on Intel Windows, AMD RDNA 2 Windows, NVK and KosmicKrisp; default
  status of the vkd3d-proton heap path in stable Proton; `maintenance10/11` content; KHR timing.

## Sources (accessed 2026-10-11)

- [d1] Agility SDK downloads, updated 2026-09-14: https://devblogs.microsoft.com/directx/directx12agility/
- [d2] SM 6.9 retail and 1.719-preview, 2026-02-26: https://devblogs.microsoft.com/directx/shader-model-6-9-retail-and-more/
- [d3] SM 6.10 preview and 1.720, 2026-04-27: https://devblogs.microsoft.com/directx/shader-model-6-10-agilitysdk-720-preview/
- [d4] Agility 1.721 preview, 2026-05-28: https://devblogs.microsoft.com/directx/announcing-agilitysdk-721-preview-and-more-shader-model-6-10-features/
- [d5] Agility 1.608, 2022-11-18: https://devblogs.microsoft.com/directx/agility-sdk-1-608-0/
- [d6] Enhanced barriers spec (incl. fence barriers): https://github.com/microsoft/DirectX-Specs/blob/master/d3d/D3D12EnhancedBarriers.md
- [d7] Revised create views spec: https://github.com/microsoft/DirectX-Specs/blob/master/d3d/D3D12RevisedCreateViews.md
- [d8] Work Lists spec v0.852, 2026-09-11: https://github.com/microsoft/DirectX-Specs/blob/master/d3d/WorkLists.md
- [d9] DirectX adopting SPIR-V, 2024-09: https://devblogs.microsoft.com/directx/directx-adopting-spir-v/
- [d10] Agility SDK announcement, 2021-04-20: https://devblogs.microsoft.com/directx/announcing-dx12agility/
- [d11] SM 6.6 dynamic resources: https://github.com/microsoft/DirectX-Specs/blob/master/d3d/HLSL_SM_6_6_DynamicResources.md
- [d12] GPU upload heaps spec: https://github.com/microsoft/DirectX-Specs/blob/master/d3d/D3D12GPUUploadHeaps.md
- [d13] Advanced Shader Delivery retail, 2026-10-01: https://devblogs.microsoft.com/directx/advanced-shader-delivery-available-for-gears-of-war-e-day-and-coming-soon-across-windows-11/
- [d14] Partial graphics programs, 2026-05-27: https://devblogs.microsoft.com/directx/partial-graphics-programs/
- [d15] Resource binding spec (limits): https://github.com/microsoft/DirectX-Specs/blob/master/d3d/ResourceBinding.md
- [h1] HLSL proposal 0010 `vk::BufferPointer`: https://github.com/microsoft/hlsl-specs/blob/main/proposals/0010-vk-buffer-ref.md
- [h2] Work graphs removed in DXIL 1.10, 2026-08-18: https://github.com/microsoft/hlsl-specs/blob/main/proposals/0018-work-graphs.md
- [h3] hlsl-specs issue 17, "Loading from Buffer Addresses": https://github.com/microsoft/hlsl-specs/issues/17
- [k1] Khronos, Roadmap 2026 and descriptor heap, 2026-01-23: https://www.khronos.org/blog/vulkan-introduces-roadmap-2026-and-new-descriptor-heap-extension
- [k2] `VK_EXT_descriptor_heap` proposal: https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_descriptor_heap.html
- [k3] Vulkan-Docs tags (v1.4.365, 2026-10-02): https://github.com/KhronosGroup/Vulkan-Docs/tags
- [k4] Vulkan roadmap appendix: https://docs.vulkan.org/spec/latest/appendices/roadmap.html
- [k5] `VK_KHR_device_address_commands` proposal: https://docs.vulkan.org/features/latest/features/proposals/VK_KHR_device_address_commands.html
- [k6] Khronos, "So long, image layouts", 2025-06-26: https://www.khronos.org/blog/so-long-image-layouts-simplifying-vulkan-synchronisation
- [k7] gpuinfo extension coverage: https://vulkan.gpuinfo.org/listextensions.php
- [k8] NVIDIA, descriptor heaps end to end, 2026-06-25: https://developer.nvidia.com/blog/streamlining-resource-binding-with-end-to-end-support-for-vulkan-descriptor-heaps/
- [k9] Registry `vk.xml` (deprecation attributes): https://github.com/KhronosGroup/Vulkan-Docs/blob/main/xml/vk.xml
- [m1] Mesa 26.2.0 release notes, 2026-08-05: https://docs.mesa3d.org/relnotes/26.2.0.html
- [v1] vkd3d-proton changelog (3.0.1, 2026-05-06): https://github.com/HansKristian-Work/vkd3d-proton/blob/master/CHANGELOG.md
- [v2] Steam survey, September 2026: https://store.steampowered.com/hwsurvey/
- [v3] Steam survey, Linux view: https://store.steampowered.com/hwsurvey/Steam-Hardware-Software-Survey-Welcome-to-Steam?platform=linux
- [v4] Steam survey, video cards: https://store.steampowered.com/hwsurvey/videocard/
- [n1] `sebbbi/NoGraphicsAPI` README and Vulkan notes, snapshot 2026-09-05: https://github.com/sebbbi/NoGraphicsAPI
- [s1] GamingOnLinux, September 2026 survey, 2026-10-02: https://www.gamingonlinux.com/2026/10/steam-survey-for-september-2026-shows-a-drop-in-linux-with-simplified-chinese-rising/
- [s4] Phoronix on DXC SPIR-V updates, 2026-02: https://www.phoronix.com/news/DX-Shader-Compiler-Better-VLK
- [s5] `OPTIONS19` fields (windows-rs docs): https://microsoft.github.io/windows-docs-rs/doc/windows/Win32/Graphics/Direct3D12/struct.D3D12_FEATURE_DATA_D3D12_OPTIONS19.html
- [s6] GPUOpen, using `D3D12_HEAP_TYPE_GPU_UPLOAD`: https://gpuopen.com/learn/using-d3d12-heap-type-gpu-upload/
- [s7] HotHardware, NVIDIA ends Maxwell/Pascal support: https://hothardware.com/news/nvidia-ends-pascal-maxwell-driver-support
- [s8] NVIDIA forum, vkd3d-proton heap path device lost on 610: https://forums.developer.nvidia.com/t/vkd3d-proton-descriptor-heap-path-causes-vk-error-device-lost-on-driver-610-in-the-first-descendant/374747
- [s9] Phoronix, Vulkan 1.4.340 with descriptor heap, 2026-01: https://www.phoronix.com/news/Vulkan-1.4.340-Descriptor-Heap
- [s11] AMD Vulkan driver release notes index (via search, not fetched): https://www.amd.com/en/resources/support-articles/release-notes/RN-RAD-WIN-VULKAN.html
- [s10] Phoronix, KosmicKrisp Vulkan 1.4 conformance, 2026-10-03: https://www.phoronix.com/news/KosmicKrisp-Vulkan-1.4-2026
