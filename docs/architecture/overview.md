# Architecture Overview

**Status**: Accepted

This page describes RojoRHI's shape as the code stands today: the v1 interface imported from
Luminex, implemented by the Metal 4 backend and frozen under `Legacy/` (ADR 0017). The top-level
`Include/`, `Source/`, `Backends/`, `Tests/` and `Shaders/` are the permanent names, reserved for
the v2 interface, which is specified in the ADRs and the API reference under `docs/spec/` and is
not implemented yet.

```
rojo-rhi/
  LICENSE  AGENTS.md  CLAUDE.md  README.md  xmake.lua  root project settings only
  .github/workflows/                          CI: policy on Linux, build/test on Metal 4 macOS
  xmake/targets.lua  setup.lua  shaders.lua   targets are includable by a host project
  Include/ Source/ Backends/ Tests/ Shaders/  the v2 interface (permanent names, no code yet)
  Legacy/Include/rojoRHI/                     v1 public headers, dependency-free
  Legacy/Source/  Legacy/Source/Base/         v1 shared implementation; private base
  Legacy/Backends/Metal4/Source/  Legacy/Backends/Metal4/ImGui/
  Legacy/Tests/  Legacy/Shaders/Tests/        v1 contract, validation and GPU conformance
  Tools/                                      public-header check, Patches/, ImGuiBufferProbe/
  Tools/Rename/  Tools/Extract/               the Luminex rename table and the extraction scripts
  docs/conventions/  docs/decisions/  docs/architecture/  docs/guides/
  docs/spec/  docs/research/                  the v2 API reference; the evidence behind it
```

## Directory responsibilities

`xmake.lua` at the root declares project settings, requires and the language level once; it is
never included directly by a host project. `xmake/` holds `targets.lua` (the includable target
declarations; the v1 targets are macOS only), a minimal `setup.lua` (metal-cpp and Slang pins), and
a test-only copy of the shader-to-metallib rule, so RojoRHI configures, builds and tests
standalone.

`Include/`, `Source/`, `Backends/`, `Tests/` and `Shaders/` are the permanent tree of the v2
interface. They hold no v2 code yet. v1 lives frozen beside them under `Legacy/` and is deleted
with the consumer port; ADR 0017 owns the layout, the freeze and the consumer's dependency rename.

`Legacy/Include/rojoRHI/` is the v1 public surface: dependency-free headers naming no backend,
windowing, ImGui or third-party type, rooted at the `rojoRHI/` include spelling ADR 0001 fixes.

`Legacy/Source/` holds the shared implementation behind those headers, including
`Legacy/Source/Base/`, the private base (assertion, alignment, JSON escaping and `std::format`
logging) that replaces the Core dependency the component had inside Luminex.

`Legacy/Backends/Metal4/Source/` is the Metal 4 backend, built through metal-cpp;
`Legacy/Backends/Metal4/ImGui/` is the optional ImGui adapter, with its maintained patch and buffer
probe under `Tools/`.

`Legacy/Tests/` and `Legacy/Shaders/Tests/` hold the contract, validation and GPU conformance
suite, including the checkpoint A cases ADR 0005 (superseded by ADR 0009) freezes by name, carried
in from Luminex's flat test tree without changing their assertions. They remain today's GPU
conformance set until the v2 tiers of ADR 0009 exist.

`Tools/` holds the public-header check that rejects an include root not spelled exactly
`rojoRHI/`, the maintained `Patches/`, and `ImGuiBufferProbe/`. `Tools/Rename/` holds the
substitution table that renamed the component inside Luminex before the extraction and stays as the
record of what that rename did; `Tools/Extract/` holds the scripts that ran the extraction itself
and the verifier that checked its result, both kept for a future re-run.

`docs/conventions/`, `docs/decisions/`, `docs/architecture/` and `docs/guides/` hold this
repository's own conventions, ADRs, architecture pages and procedures, standing alone from
Luminex's. ADRs 0002 to 0006 are the decisions the v1 code was built under: ADRs 0003 and 0006 are
superseded by ADR 0007, ADRs 0002 and 0004 by ADR 0008, and ADR 0005 by ADR 0009. They stay as
provenance.

`docs/spec/` is the API reference for the v2 interface: a conventions chapter and one chapter per
area, each entry giving semantics, valid usage and a mapping to Metal 4, D3D12 and Vulkan. It
specifies the interface and does not describe code that exists. Metal 4 is the only backend
implemented today; the D3D12 and Vulkan columns are designed mappings, since D3D12 is planned and
Vulkan is a design oracle with no code (ADR 0008).

`docs/research/` holds the dated evidence notes the decisions cite, such as
`docs/research/2026-10-11-rhi-redesign/`. Notes record what was found and are not binding;
the ADRs and the reference carry the decisions.

## Host inclusion

A consumer includes `xmake/targets.lua`, never this repository's root `xmake.lua`: xmake resolves
a project root to the outermost ancestor holding an `xmake.lua`, so the root file's project
settings, requires and language level are declared exactly once, by whichever project is
outermost.
