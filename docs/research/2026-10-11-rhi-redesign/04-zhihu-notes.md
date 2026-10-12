# Zhihu notes: RHI design and implementation posts

**Status**: Proposed

These notes summarize nine Zhihu posts (in Chinese) on RHI design, render graphs and build tooling,
archived between 2016 and 2023 and reread for the RojoRHI redesign toward Metal 4 and D3D12, kept
friendly to a later Vulkan backend. Each entry records the post's thesis, the projects it names, and
what still holds as of October 2026. The notes decide nothing; decisions live in `docs/decisions/`.

## Access log

Plain HTTP fetches returned 403 for every URL. All nine were read in full through a headless browser
without logging in. Two gaps remain: question 604591389 has 23 answers but only four render without
login (the three archived ones and 往昔's answer they rebut), and the Zhihu answers API refused (403,
code 40362). Figures were not captured: the OGRE and UE4 class diagrams in answer 128862967 and the
barrier and timeline figures in p/425830762.

| # | URL | Title | Author | Date |
|---|---|---|---|---|
| 1 | zhihu.com/question/604591389/answer/3109921208 | 如何封装一个现代游戏引擎RHI层？ | 知乎用户 (anonymous) | 2023-07-08 |
| 2 | zhuanlan.zhihu.com/p/425830762 | Render Graph与现代图形API | litmin | edited 2022-05-16 |
| 3 | zhihu.com/question/604591389/answer/3106278984 | 如何封装一个现代游戏引擎RHI层？ | SaeruHikari | 2023-07-06 |
| 4 | zhihu.com/question/604591389/answer/3108027153 | 如何封装一个现代游戏引擎RHI层？ | MaxwellGeng | 2023-07-07 |
| 5 | zhihu.com/question/56634628/answer/2706763948 | 如何评价 xmake？ | RubDuckSoftware | 2022-10-08 |
| 6 | zhuanlan.zhihu.com/p/104223699 | Direct3D12再封装(一)-----Math＆GpuResource | SaeruHikari | edited 2020-01-29 |
| 7 | zhuanlan.zhihu.com/p/492035106 | 拥抱xmake，Win C++编译新体验 | MaxwellGeng | edited 2022-04-03 |
| 8 | zhihu.com/question/30675439/answer/128862967 | 想学习游戏引擎架构用哪个开源引擎好？ | 知乎用户K9ICqz | 2016-10-28 |
| 9 | zhuanlan.zhihu.com/p/435446548 | 【Vulkan】Granite渲染框架设计思路解析 | 小小木同子 | edited 2021-11-22 |

## Context: the answer the others rebut (往昔, answer 3106010550)

Question 604591389 ("How do you wrap a modern game-engine RHI layer?") had 557 followers and 218,139
views; visible answers: MaxwellGeng (319 upvotes), SaeruHikari (275), 往昔 (131), anonymous (97). 往昔
calls an RHI "过去没有通用性跨平台方案的 legacy 产物" (a legacy of the era before cross-platform solutions). The
recommendation is to pick Vulkan and reach other platforms via porting layers (MoltenVK, VulkanOn12,
Vulkan layers), for faster feature uptake and no double-wrapping overhead. The strongest example is
a vendor GPU feature that a commercial engine's RHI will not prioritize, blocking the team.
Conclusion: "我倾向于让它不存在" (I would rather it did not exist). **2026:** the premise has weakened.
MoltenVK is active (v1.4.2, July 2026) but must squeeze Metal 4 allocators, argument tables,
residency sets and explicit barriers through Vulkan's model, and VulkanOn12 never became a
production path. The feature-latency complaint still holds, but it argues for a thin RHI with native
escape hatches, which is ADR 0003, rather than for no RHI.

## 1. Anonymous, answer 3109921208: expose common concepts, hide policy

**Thesis.** An RHI stops graphics programmers writing `VkCreateImage` and `MTL::makeTexture` twice.
It serves professionals ("而不是GAMES101票友", not hobbyists) and need not fully forbid `if (isMetal) …
else if (isVulkan)` above it. Four rules: concepts shared across APIs get one call; features one API
has and the other lacks expose their detail; conflicting implementations of one function expose only
what the engine needs; globally entangled concepts such as descriptor layout expose neither API's
model but a third derived "从GPU执行原理的视角" (from how the GPU executes). Hiding those policies in the
RHI is "组织分工的选择，而不是代码层级上的选择" (an org-chart choice, not a layering one).

**Per concept.** Device: unify the basics, keep native queries for features and limits that do not
map (max texture size is common; subpasses are not on Metal). Memory: unify size, usage and storage
mode, but keep Vulkan's memory-versus-object split visible (VMA); `VkImage` plus `VkImageView`
equals one `MTLTexture`; model the complex API and fill Metal's gaps. Shaders: syntax translation is
solved (macros, SPIRV-Cross), descriptor layout is the hard part, so expose descriptor strategies as
policies, and "如果你的图形引擎不需要 Bindless Rendering，那么就不要暴露相关的底层API细节". PSO = shader + render-pass
descriptor + render state, serializable. Render passes follow Vulkan's (the most complex), ignoring
subpasses on desktop, with framebuffers exposed for caching. Queues are Render, Compute and IO
roles, with the hardware mapping hidden. Goal by RHI kind: an engine wants low-level performance, an
emulator stability, an SDK bridge such as bgfx ease of use. Names VMA, SPIRV-Cross, ANGLE, bgfx.

**Outdated:** render-pass and framebuffer objects as the modelling base (Vulkan dynamic rendering is
core in 1.3, local read in 1.4; Metal and D3D12 were always closer to that), "PSO includes the
render pass" (attachment formats now suffice), SPIRV-Cross as the main path (Slang emits DXIL,
SPIR-V and Metal directly). **Still valid:** the four-rule split, native feature and limit queries,
queue roles over hardware queues, and one deliberate engine-wide descriptor model.

## 2. litmin, p/425830762: render graphs are how you use a modern API

**Thesis.** Explicit APIs hand scheduling to the app because "Applications know more than the
driver", and a render graph is the layer that uses that knowledge: setup (handle-based reads and
writes), compile (cull, lifetimes, cross-queue sync points), execute (insert barriers, run passes in
declaration order without reordering). Treat the GPU as a "工具人" (a tool, not a mind-reader).

**Arguments.** *Async compute:* D3D12 has 3D, compute and copy queues, and overlap pays only when
units differ (shadow rasterization against compute). On Mali, avoid fragment→compute→fragment and
overlap frame N post with frame N+1 shadows. HDRP runs light lists, SSR, SSAO, contact shadows and
volumetrics async, and SRP finds sync points by scanning for the last producer pass. *Barriers:*
execution sync (fences) versus data sync (ordering, cache flush or invalidate, layout or state
transition). Batch them, merge read-to-read states, use D3D12 promotion and decay, keep Vulkan stage
masks tight, use split barriers or events to fill bubbles, flush per dependency level. *Aliasing:*
whole-frame lifetimes let transients share memory; Frostbite went from 147 MB to about 80 MB.
*Engines (2021–22):* UE 4.27 had one RDG per desktop frame but used pooling, not aliasing; Unity SRP
had async compute and a visualizer, also without aliasing. References include Vulkan-Samples, VMA
aliasing docs, MJP's barrier series and Themaister's sync blog.

**Outdated:** the barrier vocabulary. Enhanced barriers (sync, access, layout; `SYNC_SPLIT`;
undefined or discard layout instead of aliasing barriers) supersede resource states. Metal 4 command
buffers are untracked and take explicit stage barriers. `VK_KHR_unified_image_layouts` (2025)
removes most layout transitions. The engine survey is stale: the UE5 RDG transient allocator aliases
memory, and Unity 6 makes RenderGraph the URP path with native pass merging. **Still valid:** async
compute, minimal early barriers and aliasing all need whole-frame knowledge, so they belong in a
graph above the RHI, and the RHI's job is to express precise barriers, not infer them.

## 3. SaeruHikari, answer 3106278984: design from hardware, not API text (CGPU)

**Thesis.** A good RHI must "穿透具体的平台 API 定义，透视到 GPU 的硬件执行原理和功能上" (see through the API text to how
the hardware executes) and turn vendor "私货" (embedded assumptions) into the team's own. The "U"
engines accumulated design errors that "形成通往功能的屏障" (wall off features). Study The-Forge, WebGPU (its
issues) and CGPU, the author's layer in SakuraEngine. CGPU targets D3D12, Vulkan, Metal and
consoles. A render graph drives it, so "在同步和状态操作上我们不保留任何易用性" (no convenience in sync or state), and
command interfaces do no state tracking.

**Command encoding example.** D3D12 and Vulkan allocate command memory from pools and allow mixed
recording, while Metal splits recording into per-scope encoders. Top-down designs that mimic Metal
with many lists hurt D3D12 and Vulkan. Scattered encoders do not map onto one allocator, scattered
lists do not map onto one native list, the specs want one allocator per thread and long lists, and
user-created encoders complicate management. In hardware, commands go to micro-engines that each
understand their own commands, so scope locks at begin and end of dispatch. CGPU's
`cgpu_cmd_begin_render_pass` returns the encoder and `cgpu_cmd_end_render_pass` takes it back, so
one allocator and buffer stay underneath. Render scope permits scissor, vertex and PSO commands.
Compute runs whenever bindings and barriers are valid outside a pass. Copy runs anywhere except on
the graphics engine inside a pass.

**Against porting layers.** "但凡看过 MoltenVK 或者 dxvk 代码的都不会认为那是稳妥的技术选型" (nobody who has read MoltenVK
or dxvk calls them a safe choice). One-to-one coverage forces fine-grained translated commands and
heavy submit cost. Vulkan sparse textures are too low-level for MoltenVK to cover; issue 1700,
"Sparse texture (& buffer?) support?", is still open in October 2026. Before descriptor buffers,
Vulkan indexing was "非常落后" (far behind). Unverified claim: AMD descriptor heaps and NVIDIA
multi-queue are "pure software".

**Assessment.** The method is not dated, and Metal 4 vindicates it. `MTL4CommandAllocator` with
queue-independent command buffers, pass-scoped encoders, and blit work merged into the compute
encoder match CGPU's model of allocator and buffer below, encoder from begin-pass. Descriptor
buffers and heaps and SM 6.6 heap indexing now supply the indexing model the answer found lacking.

## 4. MaxwellGeng, answer 3108027153: several layers, stateless commands (LuisaCompute)

**Thesis.** Respecting each API and unifying everything at one level conflict: Metal encoders and
Vulkan passes are begin/end scoped, while D3D12 lists are stateful. Fix: "避免使用一个RHI层，而是使用 多个层"
(several layers, not one RHI). Shaders use AST code generation instead of translation, which
decompiles and recompiles and leaves no way to tell which step failed. The serializable AST has
front ends such as a C++ DSL and Python. The runtime uses integer handles and context-free commands
("前后文无关"): `draw(a, b)` rather than `set(a); set(b); draw()`. Each command records its reads and
writes, and the backend reorders and batches after the queue, doing render-graph work there to
decouple recording from execution. Custom commands declare resources and then use native pointers
(`ID3D12GraphicsCommandList*`, `MTL::ComputePipelineState*`). Extensions shipped: DirectStorage,
OptiX denoiser, FSR2, XeSS, DirectML and DXTK. Creation callbacks let the library live inside Unity
or Blender. The backends are DX12, CUDA and Metal (SIGGRAPH Asia 2022, luisa-render.com).

**Assessment.** This is a compute or rendering framework more than a thin RHI. Context-free commands
with declared usage are the raw material of a render graph. Native-handle escape hatches and host
callbacks are worth copying, since they answer 往昔's feature-latency complaint. Slang's IR and
reflection now fill the role the AST argument played.

## 5. RubDuckSoftware, answer 2706763948: Sakura Engine's xmake build

Sakura Engine (SaeruHikari, BenzzZX) went cmake → YAML-generated cmake → cmake → xmake, now its only
build system. Its needs were declarative modules, a programmable flow, custom file types (ISPC,
HLSL, proto), reflection codegen, and incremental parallel builds. Complaints about cmake: nested
`$`, mixed configure and build scopes, several dialects, unreadable docs, slow serial custom
commands. Techniques: rules that switch plugins from DLLs in development to one static link when
shipping; `batchcmds` that put HLSL and ISPC into the parallel stream with dependency and mtime
tracking; and a libtooling-to-JSON plus mako codegen pipeline under `co_group`, more than 10x faster
than serial. **Relevance:** RojoRHI builds with xmake, and a `batchcmds` rule is the right shape for
its test-shader Slang step (ADR 0003 leaves consumer shaders to the consumer).

## 6. SaeruHikari, p/104223699: rewrapping D3D12 from MiniEngine (2020)

The author's first wrapper was built on the "龙书" (Luna's D3D12 book) demo and kept needing holes
punched in it. Commercial engines looked like "DX11 on 12". MiniEngine looked complete, thread-safe
and free of "旧图形接口时代的思想糟粕" (old-API thinking), so the author lifted it. Covered: DirectXMath
wrappers, and a `GpuResource` base tracking `D3D12_RESOURCE_STATES` and GPU VA, with
`GpuBuffer`/`PixelBuffer` deriving views through a virtual `CreateDerivedViews`, and untemplated
upload and readback buffers. Names DirectX-Graphics-Samples and misakaXunyan/DXEngineSakura (now
404). **Outdated:** per-resource state tracking, state-transition barriers, and inheritance for
resource kinds, all of which the same author dropped by 2023 in CGPU. Enhanced barriers and SM 6.6
finish them off. **Still useful:** it records how one RHI author's thinking evolved, and
MiniEngine's linear and dynamic-descriptor allocators remain clean references.

## 7. MaxwellGeng, p/492035106: xmake on Windows

cmake's abstractions (`SHARED`, `RUNTIME_LIBRARY`) hide platform reality, and its value collapses
once a team's CI sits upstream. MSBuild has no scripting and builds slowly. xmake is "cmake + ninja"
in Lua with honest docs. The post shows a `BuildProject` helper (per-mode links, clang-cl, unity
builds, post-build copy) and `xmake project -k compile_commands` for clangd, and reports full use of
24 threads at more than twice MSBuild's speed. Names MaxwellGengYF/Universal-Renderer-Descriptor
(now 404; LuisaCompute is its successor). Relevance: it supports the xmake choice, and
`compile_commands` belongs in the developer workflow.

## 8. 知乎用户K9ICqz, answer 128862967: copy OGRE, then read Unreal (2016)

The author copied OGRE 1.7.2 class by class while drawing UML, then mapped UE4. Without graphics
fundamentals neither renderer makes sense; OGRE is only a renderer; Unreal's docs are editor docs;
"框架是为了合理的使用流程而搭建的" (a framework exists to serve a pipeline you understand). There is no RHI thesis
here, and OGRE's RenderSystem is a stateful D3D9/GL-era abstraction. The quoted line is ADR 0003's
demand-driven growth in other words. A newer top answer (灰谷, 592 upvotes, 2023, edited 2025) lists
current learning engines, included in the table below.

## 9. 小小木同子, p/435446548: Granite's design trade-offs

**Thesis.** A framework balances raw API freedom against engine convenience. It is "放弃的艺术" (the art
of giving up), and wanting everything means "那还封装个锤子" (why wrap at all). Aim at the layer where 99%
of the work happens. The post paraphrases Themaister's Granite blog. *Memory:* four domains (device,
upload, cached readback, device-plus-host-visible for AMD BAR), skipping staging on integrated GPUs.
Destruction is deferred to frame retirement, and command pools are transient and reset each frame,
at the cost of memory pressure in huge frames. Per-command-buffer pools of small linear
`BufferBlock` allocators handle transient data. *Reflection:* SPIRV-Cross builds layouts because
users should not repeat the shader. *Binding:* it rejects persistent explicit sets (V-EZ, Anvil),
rejects bindless as "高端台式机的玩物，移动端只能流口水" (a desktop toy), and rejects one global pool. It picks one
pool per layout, recycling sets after 8 idle frames. Render passes are lazy, framebuffers cached,
WSI and transient layouts automatic. *Sync:* "不要试图跟踪资源使用，除非这样 做很简单" (do not track resource use
unless trivial). Tracking cannot see the future, wastes CPU on read-only data, and cannot know "last
use" under parallel recording until submit. V-EZ's record-and-patch is rejected, sync moves into the
render graph, and auto barriers stay only for WSI and transient images and static uploads.

**Outdated:** the bindless verdict (Vulkan roadmap profiles require descriptor indexing, Apple
Silicon has tier-2 argument buffers, D3D12 has SM 6.6), per-layout pools as the main model, render
pass and framebuffer caches, V-EZ itself. **Still valid:** retirement-keyed deferred destruction,
per-frame linear allocation (the idea behind ADR 0006), reflection over duplication, and the
parallel-recording argument against RHI-level tracking.

## Repository status (checked 2026-10-11 with the GitHub API)

| Repository | § | Last push | ★ | State | Repository | § | Last push | ★ | State |
|---|---|---|---|---|---|---|---|---|---|
| [SakuraEngine/SakuraEngine](https://github.com/SakuraEngine/SakuraEngine) | 3,5 | 2025-10-20 | 926 | slowing | [LuisaGroup/LuisaCompute](https://github.com/LuisaGroup/LuisaCompute) | 4 | 2026-10-10 | 1053 | active |
| [ConfettiFX/The-Forge](https://github.com/ConfettiFX/The-Forge) | 3,8 | 2026-08-27 | 5680 | active | [LuisaGroup/LuisaRender](https://github.com/LuisaGroup/LuisaRender) | 4 | 2026-07-16 | 627 | active |
| [gpuweb/gpuweb](https://github.com/gpuweb/gpuweb) | 3 | 2026-10-08 | 5499 | active | [Themaister/Granite](https://github.com/Themaister/Granite) | 9 | 2026-10-07 | 1963 | active |
| [Themaister/Granite-MicroSamples](https://github.com/Themaister/Granite-MicroSamples) | 9 | 2019-05-01 | 22 | dormant | [GPUOpen-LibrariesAndSDKs/V-EZ](https://github.com/GPUOpen-LibrariesAndSDKs/V-EZ) | 9 | 2021 (commit 2018) | 891 | abandoned |
| [microsoft/DirectX-Graphics-Samples](https://github.com/microsoft/DirectX-Graphics-Samples) | 6,8 | 2026-09-29 | 6845 | active | [KhronosGroup/Vulkan-Samples](https://github.com/KhronosGroup/Vulkan-Samples) | 2 | 2026-09-21 | 5392 | active |
| [KhronosGroup/MoltenVK](https://github.com/KhronosGroup/MoltenVK) | 3 | 2026-10-08 | 5869 | active | [KhronosGroup/SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) | 1,9 | 2026-09-28 | 2523 | active |
| [GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | 1,2 | 2026-06-04 | 3523 | active | [google/angle](https://github.com/google/angle) | 1 | 2026-10-11 | 4095 | active |
| [bkaradzic/bgfx](https://github.com/bkaradzic/bgfx) | 1 | 2026-10-09 | 17550 | active | [doitsujin/dxvk](https://github.com/doitsujin/dxvk) | 3 | 2026-10-10 | 18267 | active |
| [xmake-io/xmake](https://github.com/xmake-io/xmake) | 5,7 | 2026-10-10 | 12255 | active | [Unity-Technologies/Graphics](https://github.com/Unity-Technologies/Graphics) | 2 | 2026-09-29 | 3009 | active |
| [google/filament](https://github.com/google/filament) | 2,8 | 2026-10-09 | 20585 | active | [NVIDIAGameWorks/Falcor](https://github.com/NVIDIAGameWorks/Falcor) | 8 | 2026-09-21 | 3253 | active |
| [turanszkij/WickedEngine](https://github.com/turanszkij/WickedEngine) | 8 | 2026-10-09 | 7283 | active | [PanosK92/SpartanEngine](https://github.com/PanosK92/SpartanEngine) | 8 | 2026-10-11 | 3185 | active |
| [godlikepanos/anki-3d-engine](https://github.com/godlikepanos/anki-3d-engine) | 8 | 2026-10-07 | 1623 | active | [ezEngine/ezEngine](https://github.com/ezEngine/ezEngine) | 8 | 2026-10-10 | 2032 | active |
| [FlaxEngine/FlaxEngine](https://github.com/FlaxEngine/FlaxEngine) | 8 | 2026-10-09 | 7045 | active | [nem0/LumixEngine](https://github.com/nem0/LumixEngine) | 8 | 2026-10-10 | 3893 | active |
| [Overload-Technologies/Overload](https://github.com/Overload-Technologies/Overload) | 8 | 2026-10-05 | 2494 | active | [gongminmin/KlayGE](https://github.com/gongminmin/KlayGE) | 8 | 2026-07-17 | 2175 | slow |
| [OGRECave/ogre](https://github.com/OGRECave/ogre) | 8 | 2026-10-09 | 4671 | active | [BoomingTech/Piccolo](https://github.com/BoomingTech/Piccolo) | 8 | 2024-09-30 | 6730 | dormant |
| [AshenvaleZX/ZXEngine](https://github.com/AshenvaleZX/ZXEngine) | 8 | 2025-06-29 | 665 | slow | [79134054/VSEngine2.1](https://github.com/79134054/VSEngine2.1) | 8 | 2021-07-12 | 109 | dormant |
| misakaXunyan/DXEngineSakura | 6 | — | — | 404 | MaxwellGengYF/Universal-Renderer-Descriptor | 7 | — | — | 404 |

No repository is archived. Section 8 includes the newer 灰谷 answer; Slang is active (2026-10-11).

## Synthesis

### Philosophies that still hold

1. **Design from the hardware, not the API text** (sections 1, 3, 9). Pick a GPU function, work out
   what the hardware does, then fit each API to it. Metal 4 rewards this: allocator-backed command
   buffers with pass-scoped encoders are the model CGPU derived from hardware in 2023.
2. **No resource-state tracking in the RHI** (sections 2, 3, 9). Three independent sources give the
   same reasons: tracking cannot see the future, wastes CPU on read-only data, and breaks under
   parallel recording. Whole-frame barrier placement, async compute and aliasing belong to a graph
   above, and the RHI's job is to express precise barriers cheaply. That suits enhanced barriers,
   Metal 4 stage barriers and Vulkan sync2 alike.
3. **Common concepts plus native escape hatches.** Answer 1's four rules and LuisaCompute's
   native-pointer commands together answer 往昔's strongest point: a thin RHI must never block a
   vendor feature.
4. **The descriptor model is one deliberate choice**, mirroring no API. In 2026 the convergent
   options are argument-table slots (ADR 0003 today) or heap-indexed bindless (SM 6.6, Vulkan
   descriptor heap or buffer, Metal argument buffers). Granite's "desktop toy" verdict no longer
   holds, and the redesign should decide this explicitly rather than inherit it.
5. **Per-frame linear allocation, retirement-keyed destruction** (section 9, ADR 0006). GPU upload
   heaps on D3D12 and shared storage on Apple Silicon make write-direct the default path.
6. **Reflection over duplicated layouts** (sections 3, 9). Slang reflection now plays SPIRV-Cross's
   role.

**Aged out:** resource-state barriers with promotion, decay and aliasing barriers; Vulkan render
pass, subpass and framebuffer objects as the base; translation-first shader pipelines; per-layout
descriptor pools; bindless as desktop-only; self-tracking resource classes; porting layers instead
of an RHI; every 2021–22 engine-status claim.

### Repositories to study further

SakuraEngine CGPU (`engine/modules/engine/graphics`, plus `agent_docs/graphics/cgpu_design.md` on
the `engine` branch; check whether its Metal backend has moved to Metal 4), The-Forge (D3D12 and
Metal parity), Granite (render graph and backend trade-offs), LuisaCompute (stateless commands,
extension commands, maintained Metal backend), the WebGPU spec issues (public record of cross-API
binding and barrier debates), and WickedEngine, SpartanEngine and Falcor (modern bindless D3D12 and
Vulkan descriptor models). Skip V-EZ, Granite-MicroSamples, Piccolo, VSEngine2.1 and the two 404
repositories.

### The C-versus-C++ argument, stated fairly

The Zhihu answer does not argue the point. CGPU's reasoning lives in SakuraEngine's
`agent_docs/graphics/cgpu_design.md`, under "为什么选择 C 接口" (why a C interface). Its principle is
"零开销抽象 - 通过 C 接口和直接映射避免虚函数开销" (avoid virtual-call overhead through a C interface and direct
mapping). The four stated benefits: "ABI 稳定性 - C 接口保证跨编译器兼容" (ABI stable across compilers), "零虚函数开销
- 通过函数表而非虚表调度" (dispatch through a function table, not vtables), "易于绑定 - 可以轻松绑定到其他语言" (easy bindings
  to other languages), and "缓存友好 - 对象 大小可控，避免虚表指针" (predictable object size, no vtable pointer). In
  practice CGPU uses opaque `CGPUxxxId` handles, designated-initializer descriptor structs and a
  per-backend `CGPUProcTable`, while its backends are C++ and `cgpux.hpp` adds a C++ layer on top. C
  is the boundary language, not the implementation language. The counterweight is LuisaCompute:
  C++-first, with typed wrappers, a C++ DSL and RAII, and a C API added only for Rust and C# front
  ends.

For RojoRHI, the ABI and binding benefits are real for an RHI shipped as a binary or used from other
languages; RojoRHI is a source submodule built with its consumer's toolchain. The dispatch benefit
is weaker than it sounds, since a proc table and a vtable both cost one indirect call, and the real
saving comes from build-time backend selection, which ADR 0003 already has. The C-shaped data model
(opaque handles, plain descriptor structs, batched barrier arrays) costs nothing in C++20 and keeps
a later C shim cheap, while C++ keeps what the hard rules rely on: `Result<T>`, RAII, typed flags
and spans. The defensible position is a C++ API with C-shaped data at its boundary: no C API now,
and no choice that rules one out later.
