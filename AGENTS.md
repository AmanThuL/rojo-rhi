# RojoRHI

RojoRHI is a private, dependency-free Metal 4 rendering hardware interface (RHI) library, thin,
explicit and honest over the shared conceptual core of Metal 4, D3D12 and Vulkan (ADR
0003). Metal 4 through metal-cpp is its only backend (ADR 0002); D3D12 is a design target, not a
capability (ADR 0004). ADR 0001 owns the identity spelling every document here applies. Luminex is
its required consumer: it will mount the library as a submodule at `RojoRHI/` and drive every
change through a RojoRHI branch and pull request first ([commits](docs/conventions/commits.md)
owns that workflow). This repository holds documents only until the import; no source, build or
shader file exists yet.

## Golden sources

- Conventions: [naming](docs/conventions/naming.md), [C++ style](docs/conventions/cpp-style.md),
  [commits](docs/conventions/commits.md), [documentation](docs/conventions/documentation.md),
  [testing](docs/conventions/testing.md)
- ADRs: [0001 identity and naming](docs/decisions/0001-identity-and-naming.md),
  [0002 Metal 4 native is the first backend](docs/decisions/0002-metal4-first.md),
  [0003 thin, explicit, honest](docs/decisions/0003-thin-rhi.md),
  [0004 D3D12 is the design target](docs/decisions/0004-second-backend-target.md),
  [0005 conformance checkpoint A](docs/decisions/0005-conformance-checkpoint.md),
  [0006 per-frame data is address-first](docs/decisions/0006-execution-model.md)
- Guide: [extracting RojoRHI from Luminex](docs/guides/luminex-extraction.md)
- Architecture: [overview](docs/architecture/overview.md)

## Commands — planned, no build exists until the import

Standalone, once the import has run:

```
xmake setup   # fetches pinned metal-cpp and Slang toolchain references
xmake         # builds RojoRHI and its optional ImGui adapter
xmake test    # runs the contract and GPU conformance suite
```

Mounted inside a consumer's `RojoRHI/` checkout, the same commands take `-P .`, because xmake
resolves the project root to the outermost ancestor holding an `xmake.lua` — the consumer's root,
not this one:

```
xmake setup -P .
xmake -P .
xmake test -P .
```

GPU conformance (checkpoint A, ADR 0005) needs `MTL_DEBUG_LAYER=1` and a real device on Metal 4
Apple Silicon; there is no CPU-only substitute for it.

## Hard rules

- Metal 4 only. No Metal 3 fallback, no other backend, until a second backend is scheduled
  (ADR 0004).
- Public headers are dependency-free: no consumer header, no consumer namespace, and no backend,
  ImGui, windowing or third-party type (ADR 0003).
- A public include is rooted exactly `rojoRHI/`, in that spelling, and is written in angle form
  (ADR 0001). The case-insensitive file system will not catch a misspelling, so the check does.
- Creation returns `Result<T>`; misuse of the interface is a fatal `ROJORHI_ASSERT`.
- Every GPU object is created with a label, defaulted to the `rojorhi.` prefix when the caller
  supplies none.
- An edit made through a consumer's mounted submodule is still a RojoRHI commit: it is reviewed
  and merged here, never folded into the consumer's history.
- No AI or tool attribution in any commit message or pull request.

## Update policy

Refresh this file whenever a command, architecture contract or hard rule changes.
