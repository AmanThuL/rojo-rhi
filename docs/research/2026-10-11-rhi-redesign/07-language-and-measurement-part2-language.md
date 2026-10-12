# Implementation language, API ABI, dispatch, and overhead measurement — Part A: implementation language and API ABI

**Status**: Proposed

Part 2 of 3; the summary and the list of parts are in [07-language-and-measurement.md](07-language-and-measurement.md).

## Part A: implementation language and API ABI

### A.1 Constraints that decide the question

- One required consumer, Luminex, is C++ and builds RojoRHI from source as a submodule with the same
  compiler, standard library and flags. No binary is distributed, and nothing crosses a compiler
  boundary.
- The Metal backend is metal-cpp, a header-only C++17-minimum wrapper that Apple says has "no
  measurable overhead compared to calling Metal Objective-C headers, due to inlining of C++ function
  calls" ([metal-cpp](https://developer.apple.com/metal/cpp/)). A C implementation would have to
  move the backend to Objective-C instead, as CGPU's `.m` files do. D3D12 is COM: callable from C
  through `lpVtbl` macros, but its ecosystem is C++ (D3D12MA, Agility SDK helpers).
- The hard rules already depend on C++ features: `Result<T>`, RAII ownership, typed flags,
  `std::span` and `std::string_view` in public signatures, and `ROJORHI_ASSERT` with context.
- Desktop only: Metal 4 on Apple Silicon, then D3D12 on Windows. Vulkan is not planned (ADR 0004).
  No platform needs a runtime choice between two backends.

### A.2 C APIs in practice

| Library | Public API | Implementation | Call dispatch | Backend choice |
|---|---|---|---|---|
| SakuraEngine CGPU | C, opaque `CGPUxxxId` | C plus C++ (D3D12: `.cpp`, D3D12MA) plus Objective-C (Metal: `.m`/`.mm`) | proc table reached via `encoder->device->proc_table_cache->fn` | runtime, per instance |
| NVIDIA NRI | C and C++ from one header | C++ | caller-held interface struct of function pointers (`CoreInterface`), then a C++ member call | runtime; includes a native Metal 4 backend via metal-cpp and a `NONE` dummy backend |
| SDL3 GPU | C | C | `SDL_GPUDevice` struct of function pointers, with optional debug-mode checks per call | runtime |
| sokol_gfx | C | C (single header) | direct calls | compile time, one of `SOKOL_METAL`, `SOKOL_D3D11`, ...; `SOKOL_DUMMY_BACKEND` |
| The Forge | C-style (`cmdDraw(Cmd*, ...)`) | moving to C99 | direct calls since runtime API switching was removed | compile time |
| webgpu.h (Dawn, wgpu-native) | C | C++ / Rust | exported functions, or an optional proc table | runtime inside the implementation |
| Diligent | C++ (COM-like) with an optional C API | C++ | virtual; the C API calls through the same vtables | runtime |

Sources: CGPU [design note](https://github.com/SakuraEngine/SakuraEngine/blob/engine/agent_docs/graphics/cgpu_design.md),
[README](https://github.com/SakuraEngine/SakuraEngine/blob/engine/engine/modules/engine/graphics/include/SkrGraphics/README.md),
[`cgpu.c`](https://github.com/SakuraEngine/SakuraEngine/blob/engine/engine/modules/engine/graphics/src/common/cgpu.c),
[D3D12 sources](https://github.com/SakuraEngine/SakuraEngine/tree/engine/engine/modules/engine/graphics/src/d3d12);
[NRI README](https://github.com/NVIDIA-RTX/NRI),
[NRI `ImplD3D12.cpp`](https://github.com/NVIDIA-RTX/NRI/blob/main/Source/D3D12/ImplD3D12.cpp);
[SDL `SDL_gpu.c`](https://github.com/libsdl-org/SDL/blob/main/src/gpu/SDL_gpu.c);
[`sokol_gfx.h`](https://github.com/floooh/sokol/blob/master/sokol_gfx.h);
[The Forge releases](https://github.com/ConfettiFX/The-Forge/releases) and
[`IGraphics.h`](https://github.com/ConfettiFX/The-Forge/blob/master/Common_3/Graphics/Interfaces/IGraphics.h);
[webgpu-headers](https://github.com/webgpu-native/webgpu-headers);
[Diligent C tutorial](https://github.com/DiligentGraphics/DiligentSamples/tree/master/Tutorials/Tutorial03_Texturing-C).

**SakuraEngine's stated rationale.** No Zhihu post states it; the Zhihu material
([04](04-zhihu-notes.md) §3) covers CGPU's bottom-up design, not the language choice. The rationale
is in the repository's `agent_docs/graphics/cgpu_design.md` (last changed 2025-09-21; it sits in an
agent-documentation folder and may be a later write-up rather than the original authors' argument).
Its principle is "零开销抽象 — 通过 C 接口和直接映射避免虚函数开销" (zero-overhead abstraction by
avoiding virtual calls through a C interface). It lists four benefits:

- ABI stability across compilers;
- function-table dispatch instead of vtables;
- easy bindings to other languages;
- controllable object size with no vtable pointer.

The engine README adds that CGPU does little dirty-state caching, which "显著降低 invoke 的成本"
(significantly lowers the cost of each call).

**The claims, checked against RojoRHI:**

| Claimed benefit | Real? | Matters for RojoRHI? |
|---|---|---|
| ABI stability | Yes, for binaries crossing compilers | No. Built from source with the consumer's toolchain. |
| FFI and bindings | Yes. C is the lingua franca; Diligent and LuisaCompute add C APIs for C#/Rust front ends. | Not now. Keep a later C shim cheap instead. |
| Faster compiles | Partly. C headers parse faster. RojoRHI's whole public surface (`<rojoRHI/RHI.h>`) preprocesses to 32,403 lines and costs about 0.12 s of `-fsyntax-only`, against 0.01 s for an empty TU. | Marginal. It is paid once per including TU; measure it in CI (B.6) rather than redesign for it. |
| No hidden costs | Style, not language. C++ hides costs through allocation, copies and RTTI, all avoidable by convention. | The observed hidden costs were data-structure costs (B.1), which C does not prevent. |
| Function table instead of vtable is "zero virtual overhead" | No. CGPU's draw path is three dependent loads plus an indirect call, one more load than a vtable. The probe in A.6 measures them equal. | The real saving is direct calls, which C and C++ both get from build-time selection. |
| No vtable pointer per object | True, 8 bytes per object | Irrelevant with handle-based objects. |

A C implementation also costs RAII, templates for typed flags and handles, `std::expected`, and the
standard containers, and metal-cpp would have to be replaced by Objective-C. The Forge is the
serious counterexample: it is moving its runtime to C99 because "small teams deal better with a C99
code base" (release 1.62, February 2025). That is a team-ergonomics argument, not a performance
one, and RojoRHI's conventions and tests are already C++.

### A.3 C++ status in October 2026

| Feature | Apple clang (Xcode) | MSVC | Use in public headers? |
|---|---|---|---|
| `std::expected` | Xcode 15+; compiles on the probe machine | 19.33+ | Yes. It could back `Result<T>`. |
| Deducing `this` (P0847) | Xcode 16.3+; compiles | 19.32 partial, 19.43 | Yes, sparingly |
| `std::print` | Xcode 15.3+; compiles | 19.37+ | Not needed. The library prints nothing (ADR 0003 message callback). |
| `std::flat_map` | Xcode 26; compiles | 19.51 (Build Tools 14.51) | Implementation only, if useful |
| Named modules, `import std` | Rejected on the probe machine: `export module m;` → "expected template", `import std;` → "unknown type name 'import'". [arewemodulesyet](https://arewemodulesyet.org/tools/) also lists Apple clang without C++20 modules. | Supported since 19.35/19.36 | No |
| Language mode | `-std=c++23`, and `-std=c++26` is accepted | `/std:c++23preview` or `/std:c++latest`. Microsoft says the non-preview `/std:c++23` arrives with Build Tools 14.52 Preview. | Re-check before D3D12 work |
| C++26 static reflection | Only in a Bloomberg Clang fork | No public support or ETA | No |

Sources: [Apple C++ language support](https://developer.apple.com/xcode/cpp/),
[cppreference C++23 compiler support](https://en.cppreference.com/w/cpp/compiler_support/23),
[MSVC C++23 in Build Tools 14.51](https://devblogs.microsoft.com/cppblog/c23-support-in-msvc-build-tools-14-51),
[ReSharper C++ 2026.2 note on reflection support](https://blog.jetbrains.com/rscpp/2026/07/22/resharper-cpp-2026-2-cpp-26-reflection-ispc-language-support-and-more/).
The "compiles" entries come from a local probe that built one TU using deducing `this`,
`std::expected`, `std::println` and `std::flat_map` with `-std=c++23 -O2`.

xmake supports C++20 modules for gcc, clang and MSVC
([xmake modules](https://xmake.io/examples/cpp/cxx-modules.html)), but the compiler RojoRHI must
build with does not. Modules would also collide with metal-cpp, which is macro-driven
(`*_PRIVATE_IMPLEMENTATION`). clang-cl is a viable second Windows compiler: it is Clang, so it keeps
deducing `this` and `std::expected` parity with Apple clang, and it enables Clang ThinLTO and
`-ftime-trace` on Windows. MSVC remains the reference compiler for D3D12 because that is what PIX,
the Agility SDK samples and most Windows engines assume.

### A.4 Rust

- **Ecosystem.** [wgpu](https://github.com/gfx-rs/wgpu) is the main Rust RHI. It has no Metal 4
  backend: its open Metal issues concern the classic API, for example
  [#9414](https://github.com/gfx-rs/wgpu/issues/9414) on Metal 4-era timestamps returning zeros.
  [objc2-metal](https://github.com/madsmtm/objc2/tree/main/framework-crates/objc2-metal) generates
  `MTL4*` bindings, and [windows-rs](https://github.com/microsoft/windows-rs) covers D3D12. A Rust
  RojoRHI would be built on those, not on wgpu.
- **wgpu's own dispatch history.** wgpu 23.0.0 (2024-10-25) moved `wgpu-core` from generics over
  backends to dynamic dispatch (`DynDevice` and friends) for "faster compile times and smaller
  binaries". Its benchmarks then showed "compute pass recording is slower ... whereas on render
  passes speed improvements have been observed"
  ([CHANGELOG](https://github.com/gfx-rs/wgpu/blob/trunk/CHANGELOG.md),
  [#6002](https://github.com/gfx-rs/wgpu/pull/6002), [#5124](https://github.com/gfx-rs/wgpu/issues/5124)).
  A mature RHI judged dispatch mechanism a second-order performance factor.
- **FFI cost to a C++ consumer.** An `extern "C"` call adds no marshalling, but it is an
  optimization barrier: every RHI call becomes non-inlinable unless cross-language LTO is set up.
  That requires matching LLVM versions between rustc and clang, `lld`, and on Windows `clang-cl`
  plus `lld-link` instead of MSVC
  ([rustc linker-plugin-LTO](https://doc.rust-lang.org/rustc/linker-plugin-lto.html)).
  Apple clang's LLVM is not version-matched to rustc's, so on the primary platform the barrier is
  effectively permanent.
- **Build.** xmake builds Rust and cxx bridges (`add_requires("cargo::...")`, `rust.cxxbridge`;
  [xmake Rust](https://xmake.io/examples/other-languages/rust.html)), but it adds a second
  toolchain, a cargo dependency graph, and a C++/Rust type bridge for every public type.
- **Safety payoff.** It is small at this layer. GPU lifetime, residency and hazards are outside the
  borrow checker's view, so the backend would be mostly `unsafe` FFI anyway.

Rust fits a new engine written in Rust. For a C++ consumer it adds a permanent inlining barrier and
a second toolchain in exchange for safety the GPU boundary cannot exploit. Rejected.

### A.5 Hybrids

- **C ABI header over a C++ implementation** (NRI, CGPU, Diligent's C API). This works and is
  proven, but every call crosses a non-inlinable function pointer, and the C++ consumer pays a
  translation layer (or a second C++ wrapper on top, like CGPU's `cgpux.hpp` or NRI's C++ view). It
  is worth it only for distribution or bindings.
- **C++ header over a C ABI.** Same costs, inverted. It suits a binary SDK, not a submodule.
- **C++ header with C-shaped data.** This is recommended. Descriptors are aggregates, handles are
  trivially copyable values, and arrays are `std::span<const T>`, so an `extern "C"` facade could
  later be generated without touching the core.

### A.6 Dispatch mechanism: what it costs and what it buys

| Mechanism | Per-call work | Inlinable | Several backends in one process | Notes |
|---|---|---|---|---|
| Virtual interface (RojoRHI today, NVRHI, Diligent, Godot's `RenderingDeviceDriver`) | object → vptr → slot → indirect call | Only if devirtualized (`final` plus LTO/whole-program vtables) | Yes | Heap object per resource (`Result<std::unique_ptr<Buffer>>`), easy mocks |
| Function-pointer table (CGPU, SDL3 GPU, NRI, webgpu proc tables) | one to three dependent loads → indirect call | No | Yes | C-compatible; same cost class as virtual |
| Link-time single backend (sokol_gfx, The Forge after 1.59) | direct call | Yes, with LTO or header-inline fast paths | No; one backend per binary | Matches ADR 0003's build-time selection |
| Recorded command stream replayed by a backend (bgfx, Filament, Godot's command graph) | write a packet, later one indirect call per packet on another thread | No | Yes | Adds a copy and a thread hop. Pays off only with an engine-side render thread. |

**Measured local evidence (probe, indicative).** In one release binary, 4,096 "draws" per frame each
wrote a 16-byte command through one of three front doors to the same backend function in another
TU: a virtual call, a CGPU-style chain `e->device->procs->draw(...)`, and a non-virtual member of a
`final` class. The probe ran 2,000 timed frames per variant with rotated order over 10 counted
rounds and took the median ns/call.

| Build | Virtual | Proc-table chain | Direct |
|---|---:|---:|---:|
| `-O2`, no LTO | 1.16 | 1.16 | 1.72 |
| `-O2 -flto=thin` | 1.25 | 1.27 | 0.26 |

Without inlining, the three mechanisms land within half a nanosecond of each other, and their order
is an artifact of code layout: a monomorphic indirect branch is predicted perfectly on Apple cores.
With ThinLTO only the direct form inlines, saving about 1 ns per trivial call. For a 4,096-draw
frame that is about 4 µs.

**Luminex's measurements of the incumbent virtual-interface RHI (M3 Max):**

- The S-BIND workload costs 147.8 µs for 1,024 draws, about 144 ns per bind-bind-draw iteration. One
  extra indirect call is therefore at most about 1% of a draw. Each native call already crosses
  `objc_msgSend` in metal-cpp, and on D3D12 a COM vtable.
- M5.2 removed non-inlined cross-TU calls, a capture-recording check and three counter increments
  from `bindFrameData`. Neutralizing each from scratch builds measured a combined ≈1.7 ns per call,
  under 5% of the 35.9 ns gap the work was meant to close.
- The gap itself was the frame-data pages' write-combined CPU cache mode: 42.6 ns per call of
  uncached scattered stores, confirmed out of sample at 8× the call count.
- The large wins were structural:
  - removing per-frame buffer creation took the representative graph from 9.284 ms to 0.352 ms
    (−96.2%);
  - an O(n) `createBuffer` registry scan accounted for 11.9% to 44.6% of the dynamic baselines;
  - buffer creations fell from 279,832 to 9 over a 272-frame run.

**External evidence agrees.** The extra loader trampoline on Vulkan device calls costs "1-5% for
typical Vulkan applications" ([GPUOpen](https://gpuopen.com/reducing-vulkan-api-call-overhead/)) and
"as high as 7% depending on the driver and application" ([volk](https://github.com/zeux/volk)). That
loader path is about 25 instructions plus thunks, much heavier than one vtable hop. In tight loops
of trivial work, virtual dispatch is 3–7× slower than CRTP mostly "due to inlining"
([Bendersky](https://eli.thegreenplace.net/2013/12/05/the-cost-of-dynamic-virtual-calls-vs-static-crtp-dispatch-in-c)).
RHI calls are not trivial loops, though. Aras Pranckevičius' GfxDevice discussion calls the cost
"death by a thousand paper cuts" and concludes it is "mostly a matter of programming style" for
singleton-like devices ([aras-p](https://aras-p.info/blog/2011/02/01/the-virtual-and-no-virtual/)).
The Forge removed runtime API switching in release 1.59 (September 2024) because no shipping case
needed it. The reason given was product scope, not measured speed.

**Windows caveat.** With Control Flow Guard (`/guard:cf`), every indirect call, virtual or
function-pointer, goes through a guard check; direct calls do not
([CFG](https://learn.microsoft.com/en-us/windows/win32/secbp/control-flow-guard)). The GPUOpen
trace above shows the CFG thunk on the loader path. If the consumer ships with CFG, the indirect
forms cost more on Windows than the Mac probe shows. Measure it on Windows (B.7).

**What runtime polymorphism would buy.** Three things:

- Two backends in one process. This would only matter for a one-process A/B harness like Luminex's
  M5.1 `NoApiBench`, and M5.2 already showed that two executables built from two points in history
  work.
- Mock objects in unit tests. A link seam (a test backend linked instead of Metal 4) or a
  recording/null backend replaces them.
- Runtime D3D12-or-Vulkan selection on Windows. ADR 0004 rules this out.

None is a current need.

### A.7 Recommendation for RojoRHI

1. **C++ implementation, C++ public API, C-shaped data at the boundary.** No C ABI layer and no
   Rust. Revisit only if a non-C++ consumer is accepted. The facade would then be a generated
   `extern "C"` layer over the same handles and descriptors.
2. **`c++23`, restricted to the Apple clang ∩ MSVC subset** listed in A.3. No modules and no C++26
   features in public headers. Before the D3D12 backend starts, re-verify what xmake's
   `set_languages("c++23")` passes to MSVC and whether the non-preview `/std:c++23` has shipped.
3. **Link-time single backend, no virtual dispatch in the public API.**
   - Public types become `final` classes (or value handles plus a non-virtual command-list class)
     whose member functions are declared in `rojoRHI/` headers and defined once in the selected
     backend's sources, with no backend type in any public header.
   - Release builds enable LTO (`set_policy("build.optimization.lto", true)` in xmake, `/GL` with
     `/LTCG` on MSVC).
   - Hot paths that must inline even without LTO, such as the frame-data bump allocation that M5.2
     already header-inlined, keep a backend-neutral inline fast path in a public header, holding
     only POD state.
   - A test or null backend is a link seam selected by build option. It needs its own ADR, since
     ADR 0002 forbids any other backend; the ADR must frame it as a test double that never ships.
4. **Decide by measurement, not by this note.** Run the dispatch-ablation probe of B.5 on the
   real command list before converting. Expect a small win on draw-heavy microbenchmarks and none on
   the representative frame. The structural reasons (no per-object heap allocation, value handles,
   inlinable fast paths, simpler ownership) justify the change on their own if the data is neutral.
