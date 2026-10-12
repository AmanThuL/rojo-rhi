# Implementation language, API ABI, dispatch, and overhead measurement

**Status**: Proposed

**Research date**: 2026-10-11

Research input for the top-down RojoRHI redesign (Metal 4 now, D3D12 as the design target, Vulkan
as a design oracle). Part A asks which language the library and its public boundary should use and,
more importantly, how calls should reach the backend. Part B surveys how RHIs measure abstraction
overhead and proposes a methodology for comparing candidate designs before committing and for
guarding overhead afterwards. Evidence comes from upstream repositories and documentation (cited
inline and listed at the end), from Luminex's M5.1 and M5.2 execution-model and frame-data
experiments (named by title, since a RojoRHI document does not link into a consumer tree), and from
two small probes run for this note on an Apple M3 Max (macOS 27.0.1, Apple clang 21.0.0
`clang-2100.1.1.101`). Probe results are indicative, not scored evidence.

## Parts

This note is split into 3 files:

1. This file: summary.
2. [Part A: implementation language and API ABI](07-language-and-measurement-part2-language.md)
3. [Part B: overhead measurement methodology, open questions and sources](07-language-and-measurement-part3-measurement.md)

## Summary

- **Language.** Keep C++ for the implementation and the public API. Neither a C API, a Rust
  implementation nor a C ABI layer fixes a problem RojoRHI has. RojoRHI is a source submodule built
  with its one C++ consumer's toolchain, so ABI stability and FFI buy nothing today. Keep the public
  boundary C-shaped (opaque handles, plain aggregate descriptors, spans of PODs, no exceptions) so a
  C shim stays mechanical if a non-C++ consumer ever appears. [04](04-zhihu-notes.md) reaches the
  same position from the Zhihu material.
- **Standard.** Stay on `c++23` as `xmake.lua` sets it. Public headers use only the subset that
  Apple clang and MSVC both ship: `std::expected`, deducing `this`, `std::span`, designated
  initializers. Do not use named modules (Apple clang rejects `export module` and `import std`), and
  do not use C++26 reflection (MSVC has none). Re-check MSVC's non-preview `/std:c++23` switch when
  D3D12 work is scheduled.
- **Dispatch.** Use link-time single-backend selection with non-virtual (`final`) public types, and
  build release with ThinLTO (MSVC: `/GL` with `/LTCG`). ADR 0003 already selects the backend at
  build time, so the current virtual interface is vestigial. Its cost is lost inlining, not the
  indirect call itself. On the M3 Max a monomorphic virtual call, a CGPU-style proc-table call and a
  non-inlined direct call all cost about 1.2 ns. Inlining saves roughly 1 ns per trivial call, and
  only the direct, link-time-selected form can be inlined.
- **Priorities.** Dispatch is not where overhead lives. Luminex's measurements put the gains one to
  two orders of magnitude higher, in allocation, memory type and data structures. Examples:
  −96.2% CPU encoding from removing per-frame buffer creation, 42.6 ns per call from write-combined
  scattered stores, and an O(n) registry scan worth up to 44.6% of a baseline. The redesign should
  spend its measurement budget there.
- **Measurement.** Reuse Luminex's paired protocol: fresh process, AB/BA alternation, 12 pairs,
  per-repetition medians, paired bootstrap CIs, frozen materiality thresholds and digest parity.
  Add four things:
  - a native metal-cpp baseline, so overhead is stated against the raw API rather than only against
    the incumbent;
  - per-category microbenchmarks fitted as per-operation slopes;
  - deterministic counters (native calls emitted, bytes written, steady-state allocations);
  - a CI tier split, because hosted macOS runners have no Metal 4.

  Everything D3D12-side beyond a paper mapping needs a Windows GPU machine.
