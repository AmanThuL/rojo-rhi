# ADR 0012: C++23 subset, C-shaped interface, build-time backend

**Status**: Accepted (2026-10-12)

## Context

The v1 interface is a set of virtual classes, one heap object per resource, and a backend factory
that is chosen at build time. Two questions are open for v2: which language and standard the
library and its public boundary use, and how a call reaches the backend. The
[language and measurement note](../research/2026-10-11-rhi-redesign/07-language-and-measurement.md)
and its
[language part](../research/2026-10-11-rhi-redesign/07-language-and-measurement-part2-language.md)
record the evidence; the points that decide it:

- The only required consumer builds RojoRHI from source with its own compiler, standard library and
  flags. No binary is distributed and nothing crosses a compiler boundary, so ABI stability and
  foreign-function reach buy nothing today.
- metal-cpp is a header-only C++ wrapper with no measurable overhead over Objective-C, and D3D12
  and its helper libraries are C++. A C implementation would replace metal-cpp with Objective-C.
- Apple clang rejects named modules and `import std`, and MSVC has no C++26 reflection.
- On an Apple M3 Max, a virtual call, a function-pointer table call and a non-inlined direct call
  each cost about 1.2 ns. With link-time optimization the direct call inlines and costs about
  0.26 ns. Dispatch saves roughly 1 ns per trivial call, which is small beside allocation and data
  structure costs: the consumer's earlier address-first experiment (Luminex ADR 0010, "The RHI
  adopts an address-first per-frame data path; the object model stays") found its large wins in
  removing per-frame buffer creation and an O(n) registry scan, not in dispatch.
- The backend is already chosen at build time, so run-time polymorphism buys nothing a current
  need uses: two backends in one process, mock objects, or a run-time choice between two Windows
  APIs.

## Decision

**Language.** The implementation and the public API are C++. There is no C ABI layer and no Rust.
The public boundary stays C-shaped: opaque handles, plain aggregate descriptors, spans of plain
data, no exceptions. A generated `extern "C"` facade stays mechanical if a non-C++ consumer is ever
accepted; none is promised.

**Standard.** `c++23`, restricted to the subset Apple clang and MSVC both ship: `std::expected`,
deducing `this`, `std::span` and designated initializers. No named modules (they would also collide
with metal-cpp's macro-driven implementation switches) and no C++26 features in public headers.
Public headers name only the standard library and RojoRHI's own types. Before the D3D12 backend
starts, what xmake passes to MSVC is re-verified, along with whether the non-preview `/std:c++23`
has shipped. MSVC is the reference compiler on Windows; clang-cl is an acceptable second.

**Dispatch.** Exactly one backend is compiled per build, chosen by an xmake option: Metal 4 is the
default on macOS, D3D12 on Windows once that backend exists, and Null on any platform. There are no
virtual interfaces. Each backend's sources define the public member functions directly, so a call
inlines into the consumer under link-time optimization. Release builds enable it: ThinLTO with
Apple clang, `/GL` with `/LTCG` on MSVC.

**Type shapes.** Two kinds of public type exist, both trivially copyable.

- Objects with behaviour (`Device`, `Queue`, `CommandList`, `Swapchain`) are value wrappers around
  one backend pointer, with non-virtual member functions such as `cmd.draw(...)`.
- Resources (`Buffer`, `Texture`, `Pipeline`, `ShaderLibrary`, `Heap`, `Fence`, `Upscaler`) are
  64-bit generation-checked identifiers: a 32-bit index, a 24-bit generation and an 8-bit type tag.
  A stale or destroyed identifier asserts in non-Release (Debug) builds. Samplers have no handle and
  are reached through `SamplerIndex`.

Descriptor structs are plain aggregates with designated-initializer-friendly defaults. No backend
type appears in a public header. A hot path that must inline even without link-time optimization
may keep a backend-neutral inline fast path in a public header, holding only plain state.

**Null backend.** The Null backend implements the whole interface without a GPU: it records
commands, runs the validation tracker and maintains the counters. It is a test backend selected by
the same build option and never ships. [0008](0008-backends-and-vulkan-oracle.md) records it as the
amendment to the single-backend rule.

## Consequences

- A call into the library is a direct call that can inline. No claim is made that this changes a
  frame measurably; the structural reasons (value handles, no per-object heap allocation, simpler
  ownership) justify the change on their own, and [0014](0014-overhead-measurement.md) measures
  the result against native code rather than assuming it.
- One backend per binary. A process cannot hold two backends; an A/B harness builds two
  executables, which is how the earlier comparison ran.
- Control Flow Guard guards every indirect call on Windows and not direct ones, so build-time
  selection is at least as cheap there as on the Mac. The Windows cost is measured on the Windows
  machine, not inferred from the Mac probe.
- Compile time of the public surface is a cost paid per including translation unit. It is
  measured in hosted CI through the Null backend instead of driving a redesign of the headers.
- Rejected: a C API with a C++ wrapper (every call becomes non-inlinable and the consumer pays a
  translation layer), Rust (a permanent inlining barrier across the language boundary, a second
  toolchain, and safety the GPU boundary cannot use), a function-pointer table or virtual
  interface (the same cost class as each other, and neither inlines), and a recorded command stream
  (an added copy and thread hop that pays off only with an engine render thread).
- The standard or the dispatch rule changes only through a superseding ADR.
