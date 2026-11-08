# ADR 0029: v1 is deleted with the consumer port

**Status**: Accepted (2026-11-08)

**Supersedes**: [ADR 0017](0017-v1-frozen-under-legacy.md), whose freeze of v1 under `Legacy/`
ends here.

## Context

[ADR 0017](0017-v1-frozen-under-legacy.md) froze the v1 interface under `Legacy/` so the consumer
could keep building it until it was ported, and named the consumer port as the change that deletes
it. The consumer now builds on the v2 interface alone, through `RojoRHI` and the backend-neutral
ImGui adapter `RojoRHIImGui`. Nothing in the v2 tree borrows from `Legacy/`: no v2 file includes a
v1 header, and the one metal-cpp unit, `RojoRHIMetalCpp`, belongs to the v2 tree. The tag `v0.1.0`
names a commit that still holds v1, for a reader who needs it.

## Decision

- `Legacy/` is deleted in one commit, with the three legacy targets `RojoRHILegacy`,
  `RojoRHILegacyMetal4ImGui` and `RojoRHILegacyTests`, the v1 shader rule `xmake/shaders.lua`, the
  v1 adapter's ImGui patch and buffer probe under `Tools/`, and the `glm` package only the v1
  tests used. The v2 interface is the only interface.
- The build targets of [ADR 0001](0001-identity-and-naming.md)'s identity table are `RojoRHI`, the
  library, and `RojoRHIImGui`, the optional ImGui adapter. A host that sets
  `rojorhi_imgui_target` gets `RojoRHIImGui`, built by its plain build like `RojoRHI`.
- The freeze ends. No commit carries a `V1-fix:` trailer. The project policy check exempts the
  deleting commit, because its tree has no `Legacy/`, and its freeze check is removed afterwards,
  so the pull request that deletes `Legacy/` is still checked by it.
- The Debug-tier check of the interface is `ROJORHI_VU`, with `ROJORHI_INTERNAL_ASSERT` for library
  invariants. `ROJORHI_ASSERT`, the v1 macro, no longer exists, and the API reference names
  `ROJORHI_VU` where it named it.
- The interface version that records the deletion is `0.2.0`, tagged `v0.2.0`
  ([ADR 0022](0022-versioning-in-the-single-header.md)).

## Consequences

- A consumer that pins this commit or a later one builds the v2 interface only; a v1 build pins
  `v0.1.0`.
- Hosted CI drops its `legacy` job, which was the only job that ran the plain `xmake` build; the
  `metal4` job runs it instead.
- The format task and the layout, comment, header and shader-import tools cover the v2 tree and the
  ImGui adapter alone.
- Accepted ADRs that describe v1, such as ADR 0001's examples of v1 names, ADR 0005's checkpoint and
  ADR 0017 itself, stay as provenance; their v1 references name what was deleted here.
