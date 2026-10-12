# Standalone RHI libraries: a survey for the Metal 4 and D3D12 redesign

**Status**: Proposed

Research input for the top-down RHI redesign targeting Metal 4 and D3D12, kept friendly to a later
Vulkan backend, with Slang shaders and an interest in the *No Graphics API* model (pointer-first
memory, bindless heaps, one root pointer, stage-only barriers, minimal PSOs). It surveys open-source
rendering hardware interfaces and graphics-API abstractions that are not welded to one engine, plus
a few engine-embedded layers that are notable enough to learn from. No decision is made here;
accepted ADRs, in particular ADR 0003 and ADR 0006, remain binding.

Method: public headers were read at each project's default branch on 2026-10-11, from shallow clones
or through the GitHub API; stars, last push and licence come from the GitHub API on the same day.
Signatures in code blocks are quoted from those headers, trimmed only by elision marks. A claim that
could not be confirmed in source is marked *unverified*. "Classic Metal" means the
`MTLCommandQueue`/`MTLCommandBuffer` model of Metal 2 and 3; "Metal 4" means the `MTL4CommandQueue`,
`MTL4CommandAllocator`, `MTL4CommandBuffer` and `MTL4ArgumentTable` model introduced with macOS 26.
The companion document [01-nographicsapi.md](01-nographicsapi.md) covers Aaltonen's proposal and its
reference implementation in depth; this document places them among their peers.

## Parts

This note is split into 5 files:

1. This file: findings in brief and the comparison table.
2. [deep dives: NRI, NVRHI and FlyCube](02-standalone-rhis-part2-deep-dives-nri-nvrhi-flycube.md)
3. [deep dives: slang-rhi to NoGraphicsAPI](02-standalone-rhis-part3-deep-dives-slang-rhi-to-nographicsapi.md)
4. [shorter profiles](02-standalone-rhis-part4-shorter-profiles.md)
5. [cross-cutting analysis and sources](02-standalone-rhis-part5-analysis-and-sources.md)

## 1. Findings in brief

- **Metal 4 arrived in standalone RHIs between October 2025 and September 2026, and only in a
  handful.** NVIDIA NRI (native, through metal-cpp, alongside D3D12, D3D11, Vulkan and WebGPU),
  FlyCube (D3D12, Vulkan, Metal 4), sebbbi/NoGraphicsAPI (Vulkan 1.4, Metal 4), Zenith.NET and
  SharpGPU (C#, D3D12, Vulkan, Metal 4), loon_gpu and kiln (Vulkan, Metal 4), and Wicked Engine's
  `GraphicsDevice` (engine-embedded) use `MTL4*` command and argument-table objects. Every other
  library surveyed — wgpu, Dawn, SDL3 GPU, sokol, bgfx, LLGL, QRhi, slang-rhi, DiligentCore (closed
  Metal backend), The Forge, blade, nicegraf, IGL, MethaneKit — is still classic Metal.
- **NRI is the closest existing match to RojoRHI's target**: a C/C++ function-table interface over
  D3D12 and Metal 4 (and Vulkan), explicit enhanced-barrier vocabulary that its Metal backend
  collapses to stage masks, directly indexed descriptor heaps, device addresses, timeline fences,
  and no automatic state tracking by stated policy.
- **NoGraphicsAPI is the closest match to the pointer-first ideal** but has no D3D12 backend. No
  surveyed library combines D3D12, Metal 4 and a pointer-first root model; that combination is
  unoccupied.
- **Directly indexed descriptor heaps are now the common bindless shape**: HLSL SM 6.6
  `ResourceDescriptorHeap` on D3D12, `VK_EXT_descriptor_heap` (Vulkan 1.4.340, January 2026) or
  mutable-descriptor sets on Vulkan, and an argument buffer of resource IDs or an
  `MTLTextureViewPool` on Metal 4. NRI, NVRHI, slang-rhi, Daxa, Dawn's experimental resource table
  and NoGraphicsAPI all converge on "write a descriptor at an application-chosen index, bind the
  heap once, index in the shader".
- **Barrier models have split three ways.** Low-level libraries use the enhanced-barrier vocabulary
  (access, layout, stages per resource): NRI, XUSG Ultimate, SharpGPU, LiteFX, RPS. Pointer-first
  libraries use global stage/access barriers with a minimal layout escape: Daxa, blade,
  NoGraphicsAPI, loon_gpu, kiln, Zenith.NET. Convenience libraries track state automatically: NVRHI
  (optional), slang-rhi, DiligentCore (optional), SDL3 GPU, QRhi, wgpu-core, LLGL. FlyCube removed
  its automatic tracker in 2025.
- **Every Metal 4 backend reduces barriers to stage masks.** NRI and FlyCube both accept
  per-resource barrier descriptions and emit `barrierAfterQueueStages`/`barrierAfterEncoderStages`
  from their union. A per-resource API surface therefore costs nothing on Metal 4 and keeps the
  resource scope that D3D12 texture layouts need, which answers the "stage-pair barriers cannot
  scope to a resource" regression ADR 0006 records.
- **Root data is small and address-shaped everywhere**: 128 bytes of push data in Daxa, nicegraf and
  the WebGPU "immediates"; 256 bytes of D3D12 root signature; one GPU pointer in an argument-table
  slot on Metal 4 in NRI, FlyCube and NoGraphicsAPI. Per-frame bump allocators that return GPU
  addresses (AMD Cauldron's `DynamicBufferRing`, NoGraphicsAPI's `BumpAllocator`) are the same idea
  as ADR 0006.
- **Timeline-value synchronisation is universal** among modern designs (NRI, FlyCube, slang-rhi,
  wgpu-hal, Daxa, DiligentCore, NoGraphicsAPI); binary semaphores survive only around swapchains.
- **Two shader routes to Metal 4 compete**: Slang emitting metallib and SPIR-V/DXIL from one source
  (slang-rhi, Daxa, NoGraphicsAPI, kiln, loon_gpu, Zenith.NET), and HLSL to DXIL then Apple's Metal
  Shader Converter (NRI, FlyCube, Wicked Engine). SPIRV-Cross-to-MSL survives in older stacks
  (FlyCube's default, DiligentCore, sokol-shdc, QRhi, SDL_shadercross).
- **Hidden resource renaming is incompatible with addresses.** SDL3's `cycle`, QRhi's
  double-buffered dynamic buffers and sokol's in-flight copies keep a handle stable while the
  backing memory, and therefore its GPU address, changes. A pointer-first API must expose renaming
  as an explicit allocator, as ADR 0006 does.

## 2. Comparison table

Columns: **API** is the language and interface style; **Backends** lists Metal generation, D3D12
floor and Vulkan floor; **Resources** is how resources are named; **Binding**, **Barriers**,
**Pipelines** and **Sync** describe those models; **Activity** is stars, last push and licence on
2026-10-11.

| Library | API | Backends (Metal / D3D12 / Vulkan) | Resources | Binding | Barriers | Pipelines | Sync | Shaders | Activity |
|---|---|---|---|---|---|---|---|---|---|
| [NVIDIA NRI](https://github.com/NVIDIA-RTX/NRI) | C and C++ function tables (`CoreInterface` etc.) | **Metal 4** native + MoltenVK; D3D12 Ultimate floor (Agility SDK); VK 1.2 + sync2; also D3D11, WebGPU, none | Opaque objects; committed or placed; `GetBufferDeviceAddress` | Root constants/descriptors + descriptor sets; `DescriptorHeap` extension (SM 6.6 / `VK_EXT_descriptor_heap`) | Explicit enhanced-barrier vocabulary; no auto tracking; Metal collapses to stages | Monolithic PSO + dynamic viewport/scissor/etc.; pipeline cache | Timeline fences with values per submit | Any DXIL/SPIR-V/metallib; DXIL→MSC on Metal | 484, 2026-10-09, MIT |
| [NVRHI](https://github.com/NVIDIA-RTX/NVRHI) | C++ virtual, ref-counted handles | No Metal; D3D11, D3D12 (enhanced barriers when available), VK 1.3 | Ref-counted objects; `getGpuVirtualAddress` | Binding layouts/sets; bindless descriptor tables; volatile CBs; push constants | Automatic tracking (optional), permanent states | PSO bound via `GraphicsState` struct | `executeCommandLists` returns instance id | HLSL via ShaderMake | 2076, 2026-10-05, MIT |
| [FlyCube](https://github.com/andrejnau/FlyCube) | C++20 virtual, `shared_ptr` | **Metal 4**; D3D12 FL 11_1 legacy barriers; VK 1.2 | Ref-counted objects; placed or committed | Binding sets from HLSL register keys; bindless typed view pools | Legacy before/after states; auto tracker removed 2025; Metal → stage masks | Monolithic PSO | Timeline fences | HLSL → DXIL/SPIR-V; MSL via SPIRV-Cross or MSC | 468, 2025-12-20, MIT |
| [slang-rhi](https://github.com/shader-slang/slang-rhi) | COM-style C++, C entry point | Classic Metal (residency sets); D3D12 RS 1.1 legacy; VK 1.1–1.3; D3D11, CUDA, WebGPU, CPU | Ref-counted objects; `getDeviceAddress`; `DescriptorHandle` | Slang-reflected shader objects; bindless handles on D3D12/VK only | Automatic per encoder | PSO + lazy specialisation | Timeline fences; graphics queue only | Slang | 212, 2026-10-10, Apache-2.0 with LLVM exception |
| [SDL3 GPU](https://github.com/libsdl-org/SDL) | C ABI, opaque pointers | Classic Metal; D3D12 FL 11_0 + RB tier 2; VK 1.0 + render passes | Opaque objects; no addresses; resource "cycling" | Fixed per-stage slots; uniform push slots | Fully automatic (default-usage scheme) | Monolithic PSO | Fences per submit; single queue | SPIR-V/DXIL/DXBC/MSL/metallib; SDL_shadercross | 16772, 2026-10-10, Zlib |
| [DiligentCore](https://github.com/DiligentGraphics/DiligentCore) | C++ COM-style + generated C API | Metal closed and commercial (classic); D3D12 FL 11_0 legacy; VK 1.0+; D3D11, GL, WebGPU | Ref-counted objects; no addresses | Pipeline resource signatures + SRBs (static/mutable/dynamic); inline constants | Tracked states with `TRANSITION/VERIFY/NONE` per call | PSO + async compile + archiver | Timeline fences; deferred contexts | HLSL/GLSL/MSL/WGSL/bytecode; SPIRV-Cross | 762, 2026-10-06, Apache-2.0 |
| [The Forge](https://codeberg.org/The-Forge/The-Forge) | C99 free functions on structs | Classic Metal (tier 2 argument buffers) only on old GitHub tree; public 1.64 is D3D12 only | Structs with cached GPU VA; loader-created | Four frequency tables in one root signature (SRT) | Explicit legacy before/after, split | Monolithic PSO, global root signature | Fences + binary semaphores | FSL (HLSL superset) translator | 5680 (GitHub), 2026-08 (Codeberg), Apache-2.0 for PC only |
| [wgpu / wgpu-hal](https://github.com/gfx-rs/wgpu) | Rust; safe API over unsafe HAL traits | Classic Metal (detects Metal 4 family); D3D12 FL 11_0; VK via ash; GL | Objects; no public device address | WebGPU bind groups; binding arrays; immediates | Core: automatic; HAL: explicit per-resource usage transitions | WebGPU PSO | HAL: one timeline fence value per submit | WGSL/SPIR-V/GLSL via naga | 18249, 2026-10-11, Apache-2.0 or MIT |
| [Daxa](https://github.com/Ipotrick/Daxa) | C++20 + C API | Vulkan 1.3 only | 64-bit generational ids; BDA everywhere | One global bindless set + BDA table; 128-byte push | Global stage/access; images always GENERAL | PSO without user layouts | Timeline + binary semaphores; submit index | Slang, GLSL | 617, 2026-10-08, MIT |
| [Dawn / webgpu.h](https://github.com/google/dawn) | C ABI with chained structs | Classic Metal; D3D12 FL 11_0; VK 1.1; D3D11, GL | Opaque handles | Bind groups; experimental resource table | Automatic, validated | WebGPU PSO | Futures, `WaitAny` | WGSL via Tint | 1168, 2026-10-11, BSD-3-Clause |
| [sokol_gfx](https://github.com/floooh/sokol) | C99 single header, 32-bit ids | Classic Metal; no D3D12; VK 1.3 + descriptor buffer (experimental); D3D11, GL, WebGPU | Pooled ids; views | Fixed slots of views and samplers | Automatic | PSO | None exposed (`sg_commit`) | sokol-shdc (GLSL → all) | 10355, 2026-10-10, Zlib |
| [bgfx](https://github.com/bkaradzic/bgfx) | C++ + C99, 16-bit handles | Classic Metal; D3D12 FL 11_0–12_2; VK sync1; many more | Handles | Stage slots + named uniforms | Automatic | Implicit from 64-bit state | None exposed (`frame`) | shaderc (`.sc`) | 17550, 2026-10-09, BSD-2-Clause |
| [LLGL](https://github.com/LukasBanana/LLGL) | C++ virtual + C wrapper | Classic Metal; D3D12 FL 10_0+; VK 1.0; D3D11, GL | Objects | Heap bindings + root-style bindings | Mostly automatic | PSO with layout | Fences, single queue | Per-backend source or bytecode | 2636, 2026-10-10, BSD-3-Clause |
| [Qt QRhi](https://github.com/qt/qtbase/tree/dev/src/gui/rhi) | C++ classes, semi-public | Classic Metal; D3D12 FL 11_0 + D3D12MA; VK; D3D11, GL | Objects; double-buffered dynamic buffers | Baked shader resource bindings with dynamic offsets | Automatic at pass boundaries | PSO | Frame-based | QShader packages (qsb) | 3090, 2026-10-10, LGPL/GPL/commercial |
| [Veldrid](https://github.com/veldrid/veldrid) | C# | Classic Metal; no D3D12; VK; D3D11, GL | Objects | Resource layouts/sets | Automatic | PSO | Fences | SPIR-V cross-compile | 2706, 2026-03-17, MIT |
| [XUSG](https://github.com/StarsX/XUSG) | C++ abstract classes | D3D12 only (enhanced barriers in Ultimate) | Typed resource classes; `GetVirtualAddress` | Content-hashed descriptor tables; heap index | Per-resource tracked, emitted by caller | PSO lib cache | D3D12 fences | HLSL | 124, 2026-10-10, MIT |
| [tinyrenderers](https://github.com/chaoticbob/tinyrenderers) / [GREX](https://github.com/chaoticbob/GraphicsExperiments) | C single header / per-API samples | None / classic Metal; D3D12; VK 1.3 + descriptor buffer | Objects | One descriptor set / native | Usage transitions / native | PSO | Binary semaphores / native | HLSL, GLSL | 481 (2022) / 80 (2026-04), no licence |
| [AMD FidelityFX `FfxInterface`](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) | C function-pointer table | No Metal; D3D12 (2.x); VK only in 1.1.4 | Integer resource indices | Jobs carry SRV/UAV/CB arrays | Barrier jobs, legacy states | Effect-internal PSOs | Host's | HLSL | 1817, 2026-06-24, binary-only for 2.x core |
| [AMD RPS](https://github.com/GPUOpen-LibrariesAndSDKs/RenderPipelineShaders) | C API + RPSL graph language | No Metal; D3D12 (enhanced barriers); VK; D3D11 | Graph resources | Node callbacks record natively | Declarative access + stages; compiler derives | Host's | Batches with integer wait/signal ids | RPSL → C | 359, 2024-05-14, MIT |
| [vuk](https://github.com/martty/vuk) | C++20 render graph with futures | Vulkan only | Objects with device address | Classic sets, cached | Automatic from graph | Reflected layouts | Futures | Slang/DXC optional | 588, 2026-08-01, MIT |
| [nicegraf](https://github.com/nicebyte/nicegraf) | C API, implicit context | Classic Metal (metal-cpp); VK 1.1–1.2 | Objects | (set, binding) bind ops; 128-byte inline bytes | Automatic, opt-out per resource | PSO | Per frame | HLSL → SPIR-V/MSL | 219, 2026-10-05, MIT |
| [Nabla](https://github.com/Devsh-Graphics-Programming/Nabla) | C++20 ref-counted objects | Vulkan 1.3 profile only | Objects with device address | Descriptor sets + BDA | Explicit sync2-shaped | Vulkan PSO | Timeline semaphores | HLSL → SPIR-V | 717, 2026-10-09, Apache-2.0 |
| [blade](https://github.com/kvark/blade) | Rust, `Copy` handle structs | Classic Metal; no D3D12; VK + timeline + dynamic rendering; GLES | Plain structs | `ShaderData` derive, pushed per pass | Global; auto from pass kinds | PSO | Sync points | WGSL via naga | 808, 2026-10-09, MIT |
| [kajiya](https://github.com/EmbarkStudios/kajiya) | Rust renderer (archived) | Vulkan | Render graph | Bindless | Graph | — | — | HLSL/rust-gpu | 5338, archived 2025, Apache-2.0 |
| [Tellusim Core SDK](https://github.com/Tellusim/Tellusim_Core_SDK) | C++ (closed source, public headers) | Metal (generation unverified); D3D12; VK; D3D11, GL, WebGPU | Objects; addresses on backend subclasses | Slot calls + texture/buffer tables | Not verified | PSO | Not verified | Own toolchain | 166, 2026-08-05, proprietary |
| [sebbbi/NoGraphicsAPI](https://github.com/sebbbi/NoGraphicsAPI) | C++20 free functions | **Metal 4**; no D3D12; VK 1.4 + descriptor heap | No buffer objects: GPU heaps, raw pointers; textures in heaps | App-owned texture/sampler heaps; one root pointer per draw | Global stage/access, no resources or layouts | PSO without binding or vertex layout | Caller-owned timeline points | Slang → metallib/SPIR-V | 1617, 2026-10-10, MIT |
| [loon_gpu](https://github.com/rkevingibson/loon_gpu) | C-style C++ | **Metal 4**; VK ~1.3 | `malloc` returns GPU spans | Per-stage data pointers; texture heaps | Stage-only | PSO + separate depth state | Not verified | Slang + MSL | 104, 2026-09-21, MIT |
| [kiln](https://github.com/klukaszek/kiln) | Rust | **Metal 4**; VK 1.4 + descriptor heap | Pointers | One root pointer; handles in roots | Stage-only + hazard flags | Layout-free PSO | Not verified | Slang → metallib 4.0 | 1, 2026-10-08, no licence |
| [Zenith.NET](https://github.com/qian-o/Zenith.NET) | C# / .NET 10 | **Metal 4**; D3D12; VK 1.4 | Objects with bindless handles | Bindless handles + constant buffers | `Barrier(stages)` + separate `Transition(layout)` | PSO | Not verified | Slang → DXIL/metallib/SPIR-V | 183, 2026-10-09, MIT |
| [SharpGPU](https://github.com/haolange/SharpGPU) | C# | **Metal 4**; D3D12; VK | Committed and placed | Pipeline layout + binding table | Enhanced-barrier style, explicit | Immutable PSO | Not verified | Not verified | 71, 2026-09-23, MPL-2.0 |
| [Wicked Engine GraphicsDevice](https://github.com/turanszkij/WickedEngine) | C++ (engine-embedded) | **Metal 4**; D3D12; VK | Objects | Bindless indices + push constants | Resource-state `GPUBarrier` | PSO | Engine-managed | HLSL → DXIL → MSC | 7283, 2026-10-09, MIT |
| [IGL](https://github.com/facebook/igl) / [lightweightvk](https://github.com/corporateshark/lightweightvk) | C++ | Classic Metal; D3D12 in development; VK 1.2 / VK 1.3 bindless-only | Objects / handles | Classic / bindless + BDA | Automatic / tracked | PSO | Fences | GLSL, SPIR-V | 3236 / 484, 2026-10-11, MIT |
| [elemental](https://github.com/double-buffer/elemental) | C API | Classic Metal; D3D12; VK | Heap-placed resources | Bindless integers + push constants | Keyed by descriptor | PSO | Fences | HLSL | 3, 2026-09-11, MIT |
| [qualcomm/qhwi](https://github.com/qualcomm/qhwi) | C++ | No Metal; D3D12 enhanced barriers; VK + tile extensions | Objects | Bindless | Explicit or automatic | PSO | Not verified | Not verified | 9, 2026-08-12, BSD-3-Clause-Clear |
| [LiteFX](https://github.com/crud89/LiteFX) | C++23 | No Metal; D3D12 Agility enhanced barriers; VK 1.4 | Objects | Reflected descriptor sets | Explicit enhanced-barrier vocabulary | PSO | Timeline | HLSL/GLSL | 115, 2026-10-09, MIT |
| [MethaneKit](https://github.com/MethanePowered/MethaneKit) | C++ | Classic Metal; D3D12; VK | Objects | Program bindings | Automatic | PSO | Fences | HLSL 6 | 974, 2026-09-20, Apache-2.0 |
| [SnapRHI](https://github.com/Snapchat/SnapRHI) | C++ | Classic Metal; VK; GL/ES | Objects | Descriptor sets | Not verified | PSO | Not verified | Not verified | 45, 2026-07-22, MIT (badge) |

Metal 4 adoption timeline, from commit history and release notes: FlyCube "Metal 4 initial support"
2025-10-09; Wicked Engine macOS release with Metal 4 in version 0.72.0, January 2026 (unverified);
loon_gpu created 2026-01-05; kiln created 2026-06-08; NRI headers dated 2026 for `NRIWrapperMetal.h`
and `NRI.metal`, version 190 dated 9 October 2026; NoGraphicsAPI Metal 4 backend 2026-09-25.
