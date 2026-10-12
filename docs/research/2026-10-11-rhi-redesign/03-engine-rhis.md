# RHI layers embedded in engines: a survey for the Metal 4 and D3D12 redesign

**Status**: Proposed

Research input for the top-down RHI redesign that targets Metal 4 and D3D12, stays friendly to a
later Vulkan backend, compiles Slang and builds with xmake. It surveys the hardware abstraction
layer inside shipping engines and frameworks, whatever the engine calls it: RHI,
RenderingDevice, GAL, graphics device, backend, driver or CGPU. For each one it records the API
style, backend floors, handle model, binding model, barrier and state tracking, pipeline model,
command recording, the split with any render graph above it, the shader pipeline, and what the
authors later said about their choices. Standalone libraries such as Diligent, NVRHI, NRI, bgfx
and Qt QRhi are covered elsewhere, and appear here only where an engine is built on one. No
decision is made here; ADR 0003 and ADR 0006 remain the accepted constraints.

Method: public headers were read at their default branch tip on 2026-10-11, through shallow or
sparse clones and the GitHub API, for Godot, Filament, O3DE, SakuraEngine, Wicked Engine,
Granite, kajiya, LunaSDK, slang-rhi, Falcor, Flax, ezEngine, Lumix, Spartan, Kohi, sokol, Cocos
Creator, The Forge, RGL and SnapRHI. Unreal Engine source is gated, so its section relies on the
public API reference, the official documentation, release notes and issue tracker. Unity,
Frostbite, Traverse Research, id Software and Our Machinery are covered from documentation,
talks and blogs. A claim marked *(unverified)* rests on a secondary source or could not be
fetched. Upstream file links point at the default branch, so line positions are deliberately
not cited.

Companion files hold the detail: per-engine deep dives for
[Unreal Engine 5, Godot 4 and SakuraEngine CGPU](03-engine-rhis-ue5-godot-cgpu.md) and for
[Wicked Engine, Filament, O3DE Atom and Granite](03-engine-rhis-wicked-filament-o3de-granite.md),
and [shorter entries for the other engines plus the source list](03-engine-rhis-catalogue.md).

## 1. Findings that matter for the redesign

1. **The render graph sits above the RHI almost everywhere.** UE5 (RDG), Frostbite, Unity SRP,
   Filament, Granite, kajiya, SakuraEngine, LunaSDK and The Machinery all put the graph above
   the hardware layer. Two engines put it inside: Godot hides an implicit graph inside
   `RenderingDevice`, and O3DE puts an explicit `FrameGraph` and its compiler in the RHI. In
   every case the per-API backend stays free of graph logic and only encodes barriers and heaps
   it is told about (section 3.1).
2. **Pipeline creation hitching is the most consistent pain point.** UE built PSO precaching and
   bundled caches, Unity 6 built `GraphicsStateCollection`, Granite built Fossilize, The
   Machinery built lock-free per-thread caches, and id Tech 7 avoided the problem through content
   discipline with fewer than 500 pipelines. Engines whose PSOs are hashed lazily at draw time
   (Filament, Wicked, Granite, Lumix, Flax) all needed a pre-warm story afterwards.
3. **Binding models churn more than anything else.** Filament broke its material format to move
   from GL-style binding to descriptor sets. UE retrofitted bindless in tiers
   (`ERHIBindlessConfiguration { Disabled, RayTracing, Minimal, All }`) beside slot binding.
   ezEngine moved from slot binding to WebGPU-style bind groups. The Forge removed per-shader
   root signatures in favour of one engine-wide layout. An RHI that fixes its binding contract
   early pays for it later.
4. **A fixed engine-wide root layout is the simplest working Metal 4 and D3D12 template.** Wicked
   Engine ships one D3D12 root signature shared by every shader, mirrors it on Metal 4 as the
   top-level argument buffer of the Metal Shader Converter, and backs both with a single
   descriptor heap. Lumix, The Forge and Traverse Research's Breda use the same idea. It makes the
   backends identical at the shader ABI level, which is what a Slang-compiled shader set needs.
5. **Metal residency is where engine backends shipped bugs.** Godot shipped GPU crashes on iOS and
   visionOS from misreading `useResources:` before moving to heaps, a `MTLResidencySet` and
   untracked resources. O3DE, Filament, LunaSDK and slang-rhi still collect `useResource` calls
   per binding group. Wicked's Metal 4 backend puts every allocation in one residency set and
   commits it per submit.
6. **Implicit state tracking in the RHI has a measured cost.** wgpu-core's tracking dominated
   render-pass recording time and inserted speculative barriers between bindless dispatches. Godot
   restricts tracking to mutable resources (under 10% of them). SakuraEngine CGPU, Granite and
   Breda track nothing in the hardware layer and leave it to the graph.
7. **Abstractions shaped after one API leak.** Godot's authors say they "designed our abstraction
   around Vulkan" and then had to emulate specialization constants on D3D12 by patching DXIL.
   SakuraEngine designed around D3D12 root signatures and its Metal backend never gained render
   pipelines. Filament is still GL-shaped under its modern surface.
8. **Encoder scope matches Metal without hurting D3D12 if it is locked at begin and end of a pass.**
   SakuraEngine CGPU, LunaSDK, slang-rhi and ezEngine all return or open a pass-scoped encoder
   from a single long command buffer. CGPU's documentation explains why: one allocator per thread
   and one long list are what D3D12 and Vulkan want, and scattered Metal-style encoder objects
   fight that.

## 2. Comparison table

Floors are those the code or documentation enforces today. "Graph" says where the render graph
lives relative to the hardware layer.

| Engine | Language and style | Metal | D3D12 | Vulkan | Handles | Binding | Barriers | PSO | Graph |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Unreal Engine 5 | C++ virtual `FDynamicRHI`, deferred `FRHICommandList` replayed on RHI thread | Metal 3 class, metal-cpp since 5.4, MSC direction, SM6 on M2+ macOS 15 | SM5/SM6, root signatures, bindless tiers | SM5/SM6, RT at parity in 5.5 | Intrusive refcount `TRefCountPtr`, deletion queue | Slot parameters batched per draw, bindless retrofitted in tiers | Explicit `ERHIAccess` transitions, split barriers, mostly emitted by RDG | Monolithic, `PipelineStateCache`, precaching 5.1+ | Above (RDG) |
| Godot 4 | C++, `RenderingDevice` over stateless `RenderingDeviceDriver` | macOS 11 / iOS 14 floor, MSL 3.1, metal-cpp, MTL4 scaffolding | FL 11_0, SM 6.0, optional enhanced barriers, Mesa NIR to DXIL | 1.0 minimum, 1.2 preferred | `RID` above, pointer-cast typed IDs below | Vulkan-style uniform sets, arg buffers on Metal, tables on D3D12 | Sync1-shaped in driver; RD's graph derives them | Monolithic with render pass and spec constants, blob cache | Inside RD, implicit |
| O3DE Atom | C++, multi-device objects, pools | Arg buffer tier 1 required, tier 2 for unbounded | Legacy resource states, SM 6.0 | VMA | `Ptr<>` refcount, pool-initialized | Shader Resource Groups by frequency | Derived by RHI `FrameGraphCompiler` | `PipelineState` + `PipelineLibrary` | Inside RHI, explicit |
| Filament | C++, macro-generated `DriverAPI.inc` stream, driver thread | Default on Apple, Obj-C++, arg buffer per set per stage | none | 1.1 | 32-bit typed handles, arena | Descriptor sets (since 1.55), dynamic offsets | None in API, implicit in backends | No object; POD `PipelineState` hashed lazily | Above, lifetimes only |
| Wicked Engine | C++ virtual `GraphicsDevice`, opaque `internal_state` | Metal 4 required, metal-cpp, MSC | FL 11_0, SM 6.0, SM 6.6 heap indexing | 1.3, dynamic rendering | Structs + `shared_ptr<void>`, integer bindless index | One global root layout, slots + bindless | Explicit `GPUBarrier`; Metal 4 collapses to stage barriers | Lazy per render-pass format hash | None (manual) |
| SakuraEngine CGPU | C99, opaque `const` handles, proc tables | Partial, classic Metal, compute only | Legacy states, D3D12MA | 1.1, optional descriptor buffer | `typedef const struct X* XId` | Reflected root signature + descriptor sets by name | Manual before/after, split, queue ownership | Pipelines + experimental shader-object state buffers | Above (SkrRenderGraph) |
| Granite | C++, Vulkan types exposed | none | none | 1.1 to 1.4 | Intrusive refcount, frame contexts | GL-like slots, hashed descriptor sets | Explicit sync2 | Lazy hashed, Fossilize pre-warm | Above |
| Unity | Closed `GfxDevice`; native plugin interfaces | Metal, no plugin state handshake | Device tracks resource states | Device tracks layouts | n/a | n/a (SRP above) | Implicit tracking in GfxDevice | Lazy, `GraphicsStateCollection` warmup | Above (SRP RenderGraph) |
| Flax | C++ virtual `GPUDevice` / `GPUContext` | via MoltenVK | DX11, DX12 | yes, plus WebGPU | Objects created then `Init(desc)` | D3D11 slots (`BindSR`, `BindUA`, `BindCB`) | Implicit plus `GPUPass` hints | `GPUPipelineState` objects | None |
| ezEngine | C++ GAL, typed handles | none | none (DX11) | yes | `ezGAL*Handle` | Bind groups, pipeline layouts | Explicit batched, debug validation | Pipeline objects + cache | Above |
| Lumix | C free functions in `gpu::` | none | single global root signature | 1.0 | Pointer handles + `BindlessHandle` | Root CBV slots + bindless | Explicit per resource | Lazy hashed at draw | Above (draw streams) |
| The Forge | C API, `add*` / `cmd*` | Arg buffers, buffers 0-3 per frequency | Renderer-wide root signature | yes | Pointers | Descriptor sets by update frequency | Explicit state barriers | Pipelines | Above |
| LunaSDK | C++ COM-like, `R<Ref<T>>` results | metal-cpp, Metal 3 model | yes | yes | `Ref<>` | Vulkan-style sets | Before/after with automatic tracking | Pipeline objects | Above (RG module) |
| slang-rhi (Falcor lineage) | C++ COM, deferred command lists | Metal 3 model, tier 2 | yes | yes | `ComPtr` | Slang shader-object tree, descriptor handles | Automatic tracking, hints | Virtual pipelines resolved per list | Above |
| kajiya | Rust over ash | none | none | 1.2, RT | Graph handles | Reflected sets + bindless | `vk_sync` access types via graph | Async compiled per pass | The graph is the API |
| Bevy / wgpu | Rust, wgpu over wgpu-hal | Metal | yes | yes | Arc / IDs | WebGPU bind groups, binding arrays | Tracked in wgpu-core | WebGPU pipelines | Bevy ECS schedules |
| sokol-gfx | C, single header | Metal | none (D3D11) | 1.3 + descriptor buffer (2025) | Pool IDs | Bind slots, resource views | Lazy tracked in backend | Pipeline objects | None |
| Cocos Creator | C++ GFX, Vulkan-shaped | Metal | none | yes, plus GLES, WebGPU | Objects | Descriptor sets | Explicit `pipelineBarrier` | PSO objects | Above (frame-graph) |

## 3. Cross-cutting lessons

### 3.1 The RHI and render graph split

| Position | Engines | What the hardware layer does | What the graph does |
| --- | --- | --- | --- |
| Graph above, RHI explicit | UE5, SakuraEngine, Granite, kajiya, LunaSDK, Breda, The Machinery, Frostbite | Encodes the barriers, aliasing barriers and placements it is given; owns heaps | Lifetimes, aliasing policy, culling, async fences, barrier derivation |
| Graph above, RHI implicit | Filament, Unity, Bevy on wgpu, slang-rhi users | Tracks state itself and barriers lazily | Lifetimes, discards, culling only |
| Graph inside the RHI | Godot (implicit), O3DE (explicit) | Driver below the graph stays stateless | Same as the first row, but not optional |
| No graph | Wicked, Flax, Lumix, sokol | Explicit or tracked barriers placed by the renderer | n/a |

Three things recur. First, the graph and the backend need different vocabularies: the graph needs
access and usage declarations, the backend needs stage, access and layout (or Metal stages and
visibility), and every engine converts once at the boundary. Second, UE keeps the split clean by
placing heap and placement mechanics in the RHI (`IRHITransientResourceAllocator`) and aliasing
policy in RDG; Frostbite's backend did the same with physical or virtual aliasing. Third, putting
the graph inside the device (Godot) improves correctness for every user but loses the recording
context at replay and forces conservative full synchronization the first time a resource changes.

### 3.2 What consistently became a pain point

- **Pipeline creation** (section 1, finding 2). Engines that hash state lazily at draw time pay
  with hitches and later add capture and replay. Monolithic PSOs that include render pass formats
  multiply the count.
- **Binding model migrations**: slot binding to descriptor sets to bindless, each needing shader
  or content breaks. UE's tiered bindless and Filament's material break are the clearest cases.
- **State tracking granularity**: Godot's missing slice tracking, O3DE's deprecated stage-less
  attachments and wgpu's speculative barriers all came from tracking at the wrong grain.
- **Metal residency and argument buffers**: `useResources:` timing (Godot), iOS driver bugs (The
  Forge), the 31-slot argument buffer limit and missing buffer binding arrays (Bevy), and
  per-group `useResource` collection still present in O3DE, Filament, LunaSDK and slang-rhi.
- **Specialization constants on D3D12**: Godot's DXIL patching is the most elaborate workaround
  found; Wicked and Breda avoid the problem by using root constants and uber-shaders.
- **Mobile descriptor set limits**: O3DE merges SRGs, Granite and Breda stay within four sets.
- **Debuggability of deferred designs**: Godot's replayed graph, Filament's driver thread and
  slang-rhi's deferred lists all separate the faulty call from where it fails.

### 3.3 How engines handle Metal against D3D12

**Binding.** Three patterns appear.

1. *Per-frequency group.* O3DE SRGs, Godot uniform sets, Filament descriptor sets, The Forge
   frequencies and LunaSDK sets each become a root descriptor table on D3D12 and an argument
   buffer on Metal. This works but needs per-group residency on Metal and a root signature per
   layout on D3D12.
2. *Fixed global root layout.* Wicked, Lumix, The Forge and Breda use one root signature for every
   shader, with directly indexed heaps on SM 6.6, a few root constants and root CBVs. Wicked
   mirrors it on Metal 4 as the Metal Shader Converter's top-level argument buffer with
   descriptor heaps as buffers. This removes per-shader layout work and matches Aaltonen's
   global-heap model.
3. *Shader-object tree.* slang-rhi lets Slang's parameter layout drive both root signatures and
   argument buffers. It is the natural fit for a Slang-only shader pipeline, at the cost of a
   deferred command list.

**Residency.** Pre-Metal-4 engines collect `useResource`/`useHeap` per group or per scope. The two
Metal 4 era backends (Wicked, and Godot's barrier mode) put everything in a residency set, make
resources untracked and heap-backed, and commit the set per submit. D3D12 has no per-pass
equivalent beyond `MakeResident` and heap residency, so the residency set is a Metal-only
concern the RHI can keep internal.

**Barriers.** D3D12 engines are split between legacy states (O3DE, CGPU, Wicked, Lumix) and
optional Enhanced Barriers (Godot). On Metal, engines either rely on automatic hazard tracking
(Filament, slang-rhi, Unity) or make resources untracked and emit fences between encoders (Godot,
O3DE) or stage barriers (Wicked on Metal 4, which collapses every barrier to
`barrierAfterStages(All, All)` and ignores layouts). A precise resource barrier in the RHI is
therefore lossy on Metal 4 and needs a defined coarsening rule.

**Encoders against command lists.** CGPU, LunaSDK, ezEngine and slang-rhi lock a pass scope at
begin and end on one long command buffer, which maps to Metal encoders and to one D3D12 list per
thread without extra allocators. Godot and Filament keep Vulkan-style render pass objects instead.

**Shaders.** Two routes dominate: SPIR-V through SPIRV-Cross to MSL (Godot, Filament, O3DE,
LunaSDK) and HLSL through DXIL and the Metal Shader Converter (Wicked, Breda, and the direction
UE describes). The converter route makes the D3D12 root layout the Metal ABI, which is why it pairs
with the fixed global layout.

### 3.4 Questions this raises for RojoRHI

These are observations for the redesign discussion, not decisions; ADR 0006 keeps argument-table
slots and the object model.

- Whether a single engine-wide root layout (pattern 2) should be the D3D12 shape that mirrors the
  Metal 4 argument table, given Slang can target both and Wicked shows it working on Metal 4.
- Whether barriers stay precise in the interface and are coarsened on Metal 4, or are expressed
  at the stage-and-visibility grain that Metal 4 and Aaltonen's proposal share.
- Where a render graph would live. Every surveyed engine that kept the hardware layer explicit put
  the graph above it, with aliasing policy above and placement mechanics below.
- How pipeline capture and pre-warm will work from day one, since every lazily hashed design needed
  it later.
- Whether the encoder-scope rule from CGPU (one long command buffer, pass-scoped encoders) should be
  written into the command model before D3D12 arrives.

## 4. Open and unverified items

- UE: whether D3D12RHI uses Enhanced Barriers, the exact Metal shader path in 5.x, and the Mac
  Metal Shader Converter integration were only seen in secondary sources or search snippets.
- Unity: the internal shader translation path for Metal and Vulkan is not documented publicly.
- SakuraEngine: the reasons for the C API in the design notes come from an AI-agent documentation
  folder in the repository; the README states the C API as a feature without that list.
- Godot: the Metal 4 renderer exists only on a contributor branch at the time of writing.
- Breda: no public description of its Metal argument buffer layout was found.
- id Tech 7 pipeline counts come from secondary write-ups, not the talk itself.
