# ADR 0001: RojoRHI identity and naming

**Status**: Accepted (2026-09-19)

## Context

The component this repository holds was developed inside a consuming renderer under that
renderer's identity: namespace `lmx::rhi`, includes rooted at `RHI/`, macros prefixed `LMX_`, GPU
labels prefixed `lmx.`. Standing alone, it needs an identity of its own, fixed once and applied
mechanically, so that every call site reads the same way and a stale reference to the old identity
fails to compile rather than resolving by accident.

The project's convention elsewhere is lowercase namespaces and lowercase include roots. RHI is an
initialism the project already writes in capitals, so a strictly lowercase spelling loses the word
boundary between the name and the initialism.

## Decision

The library is RojoRHI. This ADR owns its identity, fixed as follows. Other documents cite or apply
the table; none redefines it.

| Kind | Spelling | Example |
|---|---|---|
| Namespace | `rojoRHI` | `rojoRHI::Device` |
| Backend namespace | `rojoRHI::metal4` | `rojoRHI::metal4::frameDataCounters` |
| Diagnostics namespace | `rojoRHI::debug` | the capture-schema names in `CaptureSchema.h` |
| Public include root | `rojoRHI/` | `#include <rojoRHI/Device.h>` |
| Umbrella header | `<rojoRHI/RHI.h>` | the one header that pulls in the rest |
| Macro prefix | `ROJORHI_` | `ROJORHI_ASSERT`, `ROJORHI_LOG_INFO` |
| Default GPU label prefix | `rojorhi.` | `rojorhi.buffer.unnamed` |
| Build targets | `RojoRHI`, `RojoRHIMetal4ImGui` | the core library and the optional ImGui adapter |
| Repository | `rojo-rhi` | the directory this file lives in |
| Prose name | RojoRHI | "RojoRHI exposes no backend type" |
| Mount path in a consumer | `RojoRHI/` | `RojoRHI/Include/rojoRHI/Device.h` |

The diagnostics namespace is the capture-schema nesting the component already carries; it moves
with the rest of the identity and gains no new meaning. The umbrella header keeps the basename
`RHI.h` and is included as `<rojoRHI/RHI.h>`: only the include root is renamed, so a header named
after the initialism inside a namespace named after the library reads correctly and needs no
second name.

The mixed-case namespace and include root are the one approved exception to the lowercase
convention, because RHI is an initialism and the mixed case keeps the word boundary visible. The
macro prefix is fully uppercase and the GPU label prefix fully lowercase, each following the
convention for its own kind rather than the namespace spelling.

macOS file systems are case-insensitive, and neither this repository nor its consumer compiles on a
case-sensitive host, so a misspelled include root resolves silently on every machine that builds
the library. Nothing in the build would ever report it. The public-header check therefore rejects
any include whose root is not spelled exactly `rojoRHI/`.

A call site changes as follows.

```cpp
// before                                  // after
#include "RHI/Device.h"                    #include <rojoRHI/Device.h>
lmx::rhi::Result<lmx::rhi::Buffer> b;      rojoRHI::Result<rojoRHI::Buffer> b;
LMX_ASSERT(desc.size > 0, "empty");        ROJORHI_ASSERT(desc.size > 0, "empty");
// default label "lmx.buffer.unnamed"      // "rojorhi.buffer.unnamed"
```

Rejected: all-lowercase `rojorhi`, which loses the word boundary; `rjrhi`, which loses the name;
and a `rojo::rhi` family namespace, which is longer at every call site for a family that does not
exist.

## Consequences

Every public symbol, include, macro and default label carries one spelling, so a reference to the
old identity fails loudly instead of resolving. The mixed-case exception costs a rule the
public-header check has to enforce, since the file system will not enforce it. A consumer mounting
the library at `RojoRHI/` gets no path that collides with its own component directories.
