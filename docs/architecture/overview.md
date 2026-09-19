# Architecture Overview

**Status**: Proposed

This page describes RojoRHI's shape after the import from Luminex; today the repository holds
documents only, with no source, build or shader file.

```
rojo-rhi/
  AGENTS.md  CLAUDE.md  README.md  xmake.lua  root project settings only
  xmake/targets.lua  setup.lua  shaders.lua   targets are includable by a host project
  Include/rojoRHI/                            public, dependency-free
  Source/  Source/Base/                       shared implementation; private base
  Backends/Metal4/Source/  Backends/Metal4/ImGui/
  Tests/  Shaders/Tests/                      contract, validation and GPU conformance
  Tools/                                      public-header check, Patches/, ImGuiBufferProbe/
  Tools/Rename/                               the Luminex substitution table, here today
  docs/conventions/  docs/decisions/  docs/architecture/  docs/guides/
```

## Directory responsibilities

`xmake.lua` at the root declares project settings, requires and the language level once; it is
never included directly by a host project. `xmake/` holds `targets.lua` (the includable target
declarations), a minimal `setup.lua` (metal-cpp and Slang pins), and a test-only copy of the
shader-to-metallib rule, so RojoRHI configures, builds and tests standalone.

`Include/rojoRHI/` is the public surface: dependency-free headers naming no backend, windowing,
ImGui or third-party type, rooted at the `rojoRHI/` include spelling ADR 0001 fixes.

`Source/` holds the shared implementation behind those headers, including `Source/Base/`, the
private base (assertion, alignment, JSON escaping and `std::format` logging) that replaces the
Core dependency the component had inside Luminex.

`Backends/Metal4/Source/` is the Metal 4 backend, built through metal-cpp; `Backends/Metal4/ImGui/`
is the optional ImGui adapter, with its maintained patch and buffer probe under `Tools/`.

`Tests/` and `Shaders/Tests/` hold the contract, validation and GPU conformance suite, including
the checkpoint A cases ADR 0005 freezes by name; the planned test split brings these in from
Luminex's flat test tree without changing their assertions.

`Tools/` holds the public-header check that rejects an include root not spelled exactly
`rojoRHI/`, the maintained `Patches/`, and `ImGuiBufferProbe/`. `Tools/Rename/` exists already: it
holds the substitution table that renames the component inside Luminex before the extraction, and
it stays afterwards as the record of what that rename did.

`docs/conventions/`, `docs/decisions/`, `docs/architecture/` and `docs/guides/` hold this
repository's own conventions, ADRs, architecture pages and procedures, standing alone from
Luminex's. These and `Tools/Rename/` are what the repository holds today; everything above them
arrives with the import.

## Host inclusion

A consumer includes `xmake/targets.lua`, never this repository's root `xmake.lua`: xmake resolves
a project root to the outermost ancestor holding an `xmake.lua`, so the root file's project
settings, requires and language level are declared exactly once, by whichever project is
outermost.
