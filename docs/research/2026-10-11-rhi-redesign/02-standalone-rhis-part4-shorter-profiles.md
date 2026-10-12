# Standalone RHI libraries: a survey for the Metal 4 and D3D12 redesign — shorter profiles

**Status**: Proposed

Part 4 of 5; the summary and the list of parts are in [02-standalone-rhis.md](02-standalone-rhis.md).

## 4. Shorter profiles

### 4.1 Web and portable layers

- **Dawn and webgpu.h.** [webgpu-headers](https://github.com/webgpu-native/webgpu-headers) holds the
  stable C ABI generated from a YAML source: opaque `WGPU*` handles, descriptors chained through
  `nextInChain`, `*_INIT` default macros, `WGPUStringView` labels, and futures with
  `wgpuInstanceWaitAny`. [Dawn](https://github.com/google/dawn) implements it over D3D12, D3D11,
  classic Metal, Vulkan, GL and a null backend, with Tint for WGSL. Validation is the design's
  purpose (`skip_validation` and `lazy_clear_resource_on_first_use` toggles exist). Dawn's
  experimental "sampling resource table" tracks the gpuweb [bindless
  proposal](https://github.com/gpuweb/gpuweb/blob/main/proposals/bindless.md): a sized table with
  `insert` returning a slot, `update`, `remove`, and `setResourceTable` per pass, where bindings
  that could race inside a usage scope are hidden from shaders.
- **sokol_gfx.** A single-header C99 library with one implicit context and pooled 32-bit ids. The
  2025 "views" rework made views first-class binding objects; the 2026 update rework replaced
  `sg_update_*` with transient writes and staging buffers ("quite similar to SDL3's concept of
  transfer buffers"). Its experimental Vulkan backend (December 2025) requires Vulkan 1.3,
  `EXT_descriptor_buffer`, synchronization2 and dynamic rendering; the author calls descriptor
  buffers "an absolutely perfect match" for sokol's binding model
  ([blog](https://floooh.github.io/2025/12/01/sokol-vulkan-backend-1.html)). No D3D12.
- **bgfx.** A retained, deferred design: draws go to up to 4096 numbered views, each keyed by a
  64-bit sort key, sorted at `frame()` and replayed on a render thread; each thread records through
  its own `Encoder`; all fixed-function state packs into one `uint64_t`. PSOs are implicit. Backends
  include classic Metal and D3D12 feature level 11_0 to 12_2.
- **LLGL.** C++ virtual interfaces over D3D12, D3D11, Vulkan, GL and classic Metal; a
  `PipelineLayoutDescriptor` that separates heap bindings from root-style bindings and uniforms,
  documenting the push-constant budgets (128 bytes guaranteed on Vulkan, 4 KiB on Metal `setBytes`,
  256 bytes of D3D12 root signature). Barriers are mostly automatic with a per-layout `barrierFlags`
  for storage barriers.
- **Qt QRhi.** Semi-public since Qt 6.6 (`#include <rhi/qrhi.h>` against `Qt::GuiPrivate`). Baked
  `QRhiShaderResourceBindings` with dynamic offsets; "QRhi does not expose APIs for resource
  barriers or image layout transitions"; resource updates are batched in `QRhiResourceUpdateBatch`
  objects attached to pass begin and end; dynamic buffers are transparently double-buffered.
  `QShader` packages SPIR-V, HLSL, DXIL, MSL and metallib variants with a native binding map.
- **Veldrid.** C# with resource layouts and sets, D3D11, Vulkan, GL and classic Metal; no release
  since 2023. The [NeoVeldrid](https://github.com/jhm-ciberman/neo-veldrid) fork (2026) drops native
  Metal for MoltenVK.

### 4.2 D3D12 and Vulkan specialists

- **XUSG.** D3D12-only C++ utility layer, actively maintained. Resources track per-subresource,
  per-recording-thread state and *emit* barriers into a caller array (`SetBarrier(pBarriers,
  dstState, ...)` returns the count). Descriptor tables are content-hashed and deduplicated in a
  shader-visible heap, returning either a table handle or a heap index for SM 6.6 direct indexing.
  The Ultimate module maps legacy states onto enhanced barriers with `MapBarrierState(BarrierSync&,
  BarrierAccess&, BarrierLayout&, ResourceState)` — a ready reference for the mapping table a D3D12
  backend needs. XUSG-EZ layers a D3D11-style immediate API on top.
- **LiteFX** (C++23, Vulkan 1.4 and D3D12 with the Agility SDK, enhanced barriers, reflection-built
  pipeline layouts) and **qualcomm/qhwi** (Vulkan and D3D12, bindless, explicit or automatic
  barriers, D3D12 enhanced barriers, Qualcomm tile-shading extensions) are further enhanced-barrier
  mappings.
- **vuk.** A Vulkan render graph with futures: passes are typed lambdas whose arguments declare
  access (`VUK_IA(eColorWrite)`), results are `Value<T>` futures forming an IR graph, and barriers,
  layouts and multi-queue sync are derived. Buffers carry a device address.
- **Nabla.** Vulkan-only (a "Nabla Core" profile at Vulkan 1.3 with device address, descriptor
  indexing and dynamic rendering), reference-counted `IGPU*` objects, sync2-shaped barriers,
  timeline-semaphore deferred frees, HLSL 2021 compiled to SPIR-V.
- **lightweightvk.** A bindless-only Vulkan 1.3 redesign of Meta's IGL, using descriptor indexing
  and device addresses; IGL itself (classic Metal, Vulkan 1.2, GL) has had a D3D12 backend in active
  development since late 2025 (maturity unverified).
- **kajiya.** Embark's archived Rust Vulkan GI renderer; a render graph and bindless reference
  rather than a library.

### 4.3 Middleware boundaries

- **AMD FidelityFX `FfxInterface`.** The SDK's internal backend boundary is a C struct of about
  twenty function pointers. Effects never record commands; they schedule jobs (`CLEAR_FLOAT`,
  `COPY`, `COMPUTE`, `BARRIER`, `DISCARD`) that the backend replays into the host command list at
  `fpExecuteGpuJobs`. A compute job is a self-contained bind-and-dispatch record, resources are
  plain integers registered per frame, and bindless capacity is declared up front. The 2.x core is
  binary-only and D3D12-only; 1.1.4 is MIT with D3D12 and Vulkan backends.
- **AMD Cauldron.** A D3D12/Vulkan sample framework, not an RHI (two parallel implementations). Its
  per-frame ring returns a GPU virtual address directly: `D3D12_GPU_VIRTUAL_ADDRESS
  AllocConstantBuffer(uint32_t size, const void *pInitData);` — the same contract as ADR 0006.
- **AMD Render Pipeline Shaders.** A render-graph compiler: RPSL (HLSL with node and access
  attributes) compiles to C, a runtime schedules nodes, derives barriers from declared access
  (access bits times stage mask, emitting enhanced barriers on D3D12) and aliases transient memory,
  and backends plug in through a callback table. Dormant since May 2024.

### 4.4 Rust and C pointer-first siblings

- **blade.** kvark's Rust library over Vulkan, classic Metal and GLES. Its motivation document is a
  compact manifesto: "Resource states do not exist", barriers are "only global, and there are no
  image layout changes - everything is GENERAL", bindings are pushed directly to encoders with no
  descriptor sets or pools, and resources are small `Copy` structs. Since 0.9 barriers are derived
  from pass kinds with a manual opt-out. Its bunnymark claims parity with wgpu but well below
  wgpu-hal throughput.
- **nicegraf.** A C API over Vulkan 1.1–1.2 and classic Metal with automatic hazard tracking that
  can be disabled per resource (`ngf_mark_read_only`), 128 bytes of inline encoder data, and an
  offline compiler that writes a `(set binding) : metal-slot` map into each MSL file.
- **loon_gpu**, **kiln**, **sgpu** (Jai), **no_api** (Jai). Independent *No Graphics API*
  implementations from 2026. loon_gpu (Metal 4 and Vulkan) passes one data pointer per stage —
  `cmd_draw(cmd, GpuPtr vertexDataGpu, GpuPtr fragmentDataGpu, vertexCount, instanceCount)` — where
  NoGraphicsAPI shares one root; kiln (Metal 4 and Vulkan 1.4) generates the Rust struct and its
  Slang twin from one macro so host and shader layouts cannot drift, and refuses pipelines whose
  register use Metal would silently drop.

### 4.5 D3D12 + Metal 4 object-model libraries

- **Zenith.NET.** C#/.NET 10 over D3D12, Metal 4 and Vulkan 1.4; objects expose bindless handles;
  shaders are Slang compiled to DXIL, metallib and SPIR-V. It separates a stage-only
  `Barrier(BarrierStages before, BarrierStages after)` from `Transition(Texture, subresource,
  TextureLayout before, TextureLayout after)`.
- **SharpGPU.** C# over D3D12, Vulkan and Metal 4; D3D12-shaped (committed and placed resources,
  pipeline layouts, enhanced-barrier style barriers with 13 texture layouts) with a stated "fail
  closed" policy: no fallbacks, unsupported capabilities throw.
- **Wicked Engine GraphicsDevice.** Engine-embedded, but its Metal 4 backend (`MTL4::CommandQueue`,
  argument tables, counter heaps, MTL4 acceleration-structure descriptors) runs HLSL through DXIL
  and Metal Shader Converter, proving that route at production scale. Bindless indices via
  `GetDescriptorIndex` plus push constants; resource-state barriers.
- **elemental.** A small C API over D3D12, Vulkan and classic Metal with heap-placed resources,
  bindless integers and push constants; its stated direction includes GPU virtual memory.
- **Tellusim Core SDK.** Closed-source but with public headers; slot calls plus texture and buffer
  tables for bindless; device addresses only on backend subclasses.
- **MethaneKit**, **SnapRHI**, **IGL**: classic object models over classic Metal; SnapRHI's
  compile-time validation removal (`if constexpr` tags), `final` backend classes for
  devirtualisation and an "unretained resources" mode are small ideas worth noting.
