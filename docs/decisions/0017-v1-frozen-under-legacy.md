# ADR 0017: v1 is frozen under `Legacy/` until the consumer port

**Status**: Accepted (2026-10-13)

## Context

The v2 interface and the v1 interface both declare `<rojoRHI/Device.h>` and friends in namespace
`rojoRHI`, and the consumer keeps building v1 until it is ported. The v2 code needs a place to live
now, v1 needs to stop changing except for defects the consumer cannot wait for, and permanent code
must not carry a version marker that a later change strips out. The code that is going away carries
the marker instead.

## Decision

The new interface takes the final names: `Include/`, `Source/`, `Backends/`, `Tests/`, `Shaders/`
and the targets `RojoRHI` and `RojoRHITests`. v1 moves, unchanged, under `Legacy/`:

| | New interface (permanent) | v1 (frozen, deleted with the consumer port) |
|---|---|---|
| Public headers | `Include/rojoRHI/` | `Legacy/Include/rojoRHI/` |
| Shared implementation | `Source/` | `Legacy/Source/` |
| Backends | `Backends/` | `Legacy/Backends/Metal4/` with its ImGui adapter |
| Tests and test shaders | `Tests/`, `Shaders/Tests/` | `Legacy/Tests/`, `Legacy/Shaders/Tests/` |
| Targets | `RojoRHI`, `RojoRHITests` | `RojoRHILegacy`, `RojoRHILegacyMetal4ImGui`, `RojoRHILegacyTests` |

- Both trees keep the include spelling `<rojoRHI/...>` that ADR 0001 fixes. Each target's include
  directory resolves it, and no translation unit sees both trees, because no target depends on both
  libraries.
- The move is one commit of pure renames plus the path edits that make the build, the tools and CI
  find the moved files. Every moved C++ file is token-identical to its source, and every test case
  keeps its name and tags.
- The consumer port deletes `Legacy/` and the three legacy target blocks. No permanent file,
  include, namespace or target is renamed.
- `xmake/targets.lua` stays the single file a host includes. It declares the legacy targets on
  macOS only.
- From the move onwards v1 changes only to fix a defect, and the project policy check enforces the
  shape of such a change on the commits of a pull request. A checked commit changes no path outside
  `Legacy/` except `xmake/targets.lua`, and carries a trailer `V1-fix: <defect>` describing the
  defect. The move commit is exempt because its parent has no `Legacy/`. Whether a change really
  is a defect fix is judged in review.
- v1 keeps building on hosted macOS, its CPU cases keep running there, and its GPU cases keep
  running on Metal 4 hardware before a v1 fix merges.

## Consequences

- The consumer's next submodule bump renames its dependency from `RojoRHI` to `RojoRHILegacy` and
  from `RojoRHIMetal4ImGui` to `RojoRHILegacyMetal4ImGui`. Its includes do not change. A bump that
  misses the rename fails at compile time, because `RojoRHI` is then the new interface.
- The v2 code owns its own private base in `Source/Base/`. v1's base moves to
  `Legacy/Source/Base/` and is not shared.
- A Windows configure builds the new interface alone, because the legacy targets exist on macOS
  only.
- The format task and the layout, comment, header and shader-import tools cover both trees until
  `Legacy/` is deleted.
