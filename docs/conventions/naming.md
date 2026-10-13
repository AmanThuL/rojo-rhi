# Naming Conventions

**Status**: Accepted

[ADR 0001](../decisions/0001-identity-and-naming.md) owns RojoRHI's identity. This file applies that
identity as day-to-day rules and adds the names the ADR does not cover. Where the two could be read
differently, ADR 0001 wins; nothing here may contradict its table.

## Identity

| Kind | Spelling | Example |
|---|---|---|
| Namespace | `rojoRHI` | `rojoRHI::Device` |
| Backend namespace | `rojoRHI::metal4` | `rojoRHI::metal4::frameDataCounters` |
| Diagnostics namespace | `rojoRHI::debug` | the capture-schema names in `CaptureSchema.h` |
| Public include root | `rojoRHI/` | `#include <rojoRHI/Device.h>` |
| Umbrella header | `<rojoRHI/RHI.h>` | keeps the name `RHI.h`; only its root is renamed |
| Macro prefix | `ROJORHI_` | `ROJORHI_ASSERT`, `ROJORHI_LOG_INFO` |
| Default GPU label prefix | `rojorhi.` | `rojorhi.buffer.unnamed` |
| Build targets | `RojoRHI`, `RojoRHITests` | the v2 library and its tests (ADR 0017) |
| Frozen v1 targets | `RojoRHILegacy`, `RojoRHILegacyMetal4ImGui`, `RojoRHILegacyTests` | the v1 library, its ImGui adapter and its tests (ADR 0017, which amends ADR 0001's target row) |
| Repository | `rojo-rhi` | the directory this file lives in |
| Prose name | RojoRHI | "RojoRHI exposes no backend type" |
| Mount path in a consumer | `RojoRHI/` | `RojoRHI/Include/rojoRHI/Version.h` |

Namespaces are otherwise lowercase; `rojoRHI` and the matching include root are the one approved
exception, because RHI is an initialism and the mixed case keeps the word boundary visible. The
macro prefix is fully uppercase and the label prefix fully lowercase, each following the convention
for its own kind rather than the namespace spelling.

macOS file systems are case-insensitive, and nothing here builds on a case-sensitive host, so a
misspelled include root resolves silently on every machine that builds the library and the build
never reports it. The rule is therefore checked rather than trusted: an include whose root is not
spelled exactly `rojoRHI/` is rejected. That check is part of the public-header check and arrives
with the code import.

## Identifiers

- `PascalCase` types; `camelCase` functions and variables; `kPascalCase` compile-time constants.
- Private data members take the `m_` prefix (`m_device`). Public and aggregate members take no
  prefix, and there is no `s_` prefix anywhere.
- Enumerators are `PascalCase` and name the condition, not the mechanism (`DeviceUnsupported`).
- Experimental code uses `rojoRHI::experimental::<name>` and stays off `main`.

## Files and directories

- One primary type per header, in `PascalCase.h` and `PascalCase.cpp` named after it.
- Directory names are `PascalCase`: `Include`, `Source`, `Backends/Metal4`, `Tests`, `Shaders`,
  `Tools`, and `Legacy` for the frozen v1 tree, which repeats the same layout beneath it (ADR
  0017). Public headers live under `Include/rojoRHI/`; v1's private base lives in
  `Legacy/Source/Base/`, and the new interface's own `Source/Base/` arrives with the Null backend.
- Backend files carry the backend as a prefix (`Metal4Device.cpp`), so a file name alone says which
  backend owns it.
- Shader basenames are globally unique including case, per test target; a collision is rejected
  rather than resolved by directory.

## GPU labels

Default labels are lowercase, dot-separated, and describe the object kind then its state:
`rojorhi.buffer.unnamed`, `rojorhi.texture.placed.unnamed`, `rojorhi.pass.unnamed`. A
caller-supplied label is used verbatim and keeps the caller's own prefix; RojoRHI never rewrites it.

## Tests

- A `TEST_CASE` name is a lowercase sentence stating the observable behaviour, not the function
  under test: "a Greater depth test keeps the nearer fragment in reversed-Z".
- Tags are lowercase in brackets: `[contract]` for the v2 CPU contract cases in `Tests/`, `[abi]`
  for the v2 shader ABI compile and layout cases, `[rhi]` for the v1 CPU contract and validation
  cases, `[gpu]` for cases needing a real device, `[checkpoint-a]` for the frozen conformance set,
  `[.]` for a case hidden from the default run. A case carries its own tags plus any set tag it
  belongs to.

## Process environment

RojoRHI reads no environment variable and owns no environment-variable name. Where its text names
a consumer's variable — in a diagnostic message or a comment — it keeps the consumer's spelling and
is not renamed into the `ROJORHI_` family, because the process environment belongs to whoever
launches the binary.

## Differences from Luminex

- **Namespace case.** Luminex requires lowercase namespaces and include roots. RojoRHI uses
  `rojoRHI` and `rojoRHI/`, the one approved exception, because RHI is an initialism and the mixed
  case preserves the word boundary. ADR 0001 records the decision and the rejected alternatives.
- **Prefixes.** `LMX_` macros, the `lmx.` label prefix and the `lmx::rhi` namespace become
  `ROJORHI_`, `rojorhi.` and `rojoRHI`, so a stale reference to the old identity fails to compile
  instead of resolving by accident.
- **Include form.** Luminex includes its RHI headers as quoted project includes rooted at `RHI/`.
  RojoRHI's public headers are angle includes rooted at `rojoRHI/`, which is what a consumer
  mounting the library sees.
- **Shader naming.** Luminex's rules for production shader globals and entry points are dropped;
  RojoRHI has only test oracles, so the uniqueness rule is all that carries over.
