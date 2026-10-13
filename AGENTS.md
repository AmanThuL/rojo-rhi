# RojoRHI

RojoRHI is a dependency-free C++23 rendering hardware interface (RHI) library, thin, explicit and
honest over the shared conceptual core of Metal 4, D3D12 and Vulkan. Metal 4 through metal-cpp is
the only backend implemented today; D3D12 is planned and Vulkan is a design oracle with no code
(ADR 0008). The v1 interface is what the code implements now; v2 is specified in the decisions and
the API reference below, not yet implemented. ADR 0001 owns the identity spelling every document
here applies. Luminex is its required consumer: it will mount the library as a submodule at
`RojoRHI/` and drive every change through a RojoRHI branch and pull request first
([commits](docs/conventions/commits.md) owns that workflow). The repository is public and licensed
under the Apache License, Version 2.0 ([`LICENSE`](LICENSE)); it holds the library itself — public
headers, shared implementation, the Metal 4 backend, the optional ImGui adapter, tests and shaders
— alongside its founding documents (see [architecture](docs/architecture/overview.md) for
directory responsibilities).

## Golden sources

- Conventions: [naming](docs/conventions/naming.md), [C++ style](docs/conventions/cpp-style.md),
  [commits](docs/conventions/commits.md), [documentation](docs/conventions/documentation.md),
  [testing](docs/conventions/testing.md)
- ADRs: [0001 identity and naming](docs/decisions/0001-identity-and-naming.md),
  [0007 address-first interface](docs/decisions/0007-address-first-interface.md),
  [0008 backends and the Vulkan oracle](docs/decisions/0008-backends-and-vulkan-oracle.md),
  [0009 conformance and valid usage](docs/decisions/0009-conformance-and-valid-usage.md),
  [0010 the hardware floor](docs/decisions/0010-hardware-floor.md),
  [0011 one shader ABI for every backend](docs/decisions/0011-shader-abi.md),
  [0012 language and dispatch](docs/decisions/0012-language-and-dispatch.md),
  [0013 RojoRHI creates no threads](docs/decisions/0013-threading.md),
  [0014 overhead is measured against native code](docs/decisions/0014-overhead-measurement.md),
  [0015 versioning](docs/decisions/0015-versioning.md),
  [0016 C++ format follows the No Graphics API style](docs/decisions/0016-cpp-format.md),
  [0017 v1 is frozen under Legacy/ until the consumer port](
  docs/decisions/0017-v1-frozen-under-legacy.md),
  [0018 the root block reaches D3D12 as a root SRV, and the module owns the bindings](
  docs/decisions/0018-root-srv-and-reserved-bindings.md)
- API reference: [docs/spec/](docs/spec/00-conventions.md), entered through its conventions
  chapter
- Research: [docs/research/](docs/research/2026-10-11-rhi-redesign/01-nographicsapi.md), the
  evidence notes behind the decisions
- Guide: [extracting RojoRHI from Luminex](docs/guides/luminex-extraction.md)
- Architecture: [overview](docs/architecture/overview.md)

## Commands

Standalone:

```
xmake setup   # fetches pinned metal-cpp and Slang on macOS, Slang alone on Windows
xmake         # builds the frozen v1 library, RojoRHILegacy
xmake test RojoRHILegacyTests/unit   # runs the v1 CPU contract cases
xmake test RojoRHITests/contract     # runs the v2 CPU contract cases
xmake test RojoRHITests/abi          # compiles the ABI conformance shaders, checks shared layouts
xmake f --rojorhi_backend=null       # selects the v2 backend; null is the only value yet
```

The v1 GPU conformance suite runs from its test directory, after `xmake build RojoRHILegacyTests`:

```
cd build/macosx/arm64/debug/rojorhi-legacy-test
MTL_DEBUG_LAYER=1 ./RojoRHILegacyTests "[gpu]~[.]"
```

Mounted inside a consumer's `RojoRHI/` checkout, the same commands take `-P .`, because xmake
resolves the project root to the outermost ancestor holding an `xmake.lua` — the consumer's root,
not this one:

```
xmake setup -P .
xmake -P .
xmake test RojoRHILegacyTests/unit -P .
xmake test RojoRHITests/contract -P .
```

GPU conformance today is checkpoint A, the cases tagged `[checkpoint-a]`
([ADR 0005](docs/decisions/0005-conformance-checkpoint.md)); it needs `MTL_DEBUG_LAYER=1` and a real
device on Metal 4 Apple Silicon, and there is no CPU-only substitute for it. ADR 0009 defines the
conformance tiers that replace it for the v2 interface.

The shader ABI module `Shaders/RojoRHI/RojoRHI.slang` and its hosted checks exist: the conformance
shaders under `Shaders/Tests/` compile to MSL, SPIR-V and HLSL on every host, and the build checks
the layout of the shared structs against Slang reflection. That is the hosted half of the T3 tier
(ADR 0009). DXIL compiles wherever a DXC loads, as on hosted Windows CI. Compile-and-run, the ABI
version stamp and `Device::loadShaderLibrary` are not implemented; they arrive with the v2 backends.

## Hard rules

- Creation returns `Result<T>`; misuse of the interface is a fatal `ROJORHI_ASSERT`, unconditional
  in every build ([C++ style](docs/conventions/cpp-style.md)). That stays the current rule until
  the Null-backend stage brings the v2 Debug and Validation tiers (ADR 0009).
- Every GPU object is created with a label, defaulted to the `rojorhi.` prefix when the caller
  supplies none.
- Metal 4 is the only backend implemented today. D3D12 is planned and Vulkan is a design oracle
  with no code; no document claims either as a capability until it is real. The Null backend is
  test-only and arrives with its stage (ADR 0008).
- Rules for v2 work: the v2 interface is address-first and has no binding slots (ADR 0007); it is
  a C++23 subset with one backend per build, chosen by an xmake option, and no virtual interfaces
  (ADR 0012). The v1 code still has binding slots and virtual interfaces until it is ported to v2.
- v1 lives frozen under `Legacy/` until the consumer port deletes it (ADR 0017): its code moved
  unchanged, and a change there is a defect fix only, in a commit that touches nothing outside
  `Legacy/` except `xmake/targets.lua` and carries a `V1-fix:` trailer. The commit that deletes
  `Legacy/` is exempt. The project policy check enforces it.
- The module owns every shader binding and each shader compile defines exactly one
  `ROJORHI_TARGET_*` macro; a consumer shader declares no binding (ADR 0018). Changing a reserved
  binding bumps `ROJORHI_SHADER_ABI_VERSION`.
- Public headers are dependency-free: no consumer header, no consumer namespace, and no backend,
  ImGui, windowing or third-party type (ADR 0007, ADR 0012).
- A public include is rooted exactly `rojoRHI/`, in that spelling, and is written in angle form
  (ADR 0001). The case-insensitive file system will not catch a misspelling, so the check does.
- An edit made through a consumer's mounted submodule is still a RojoRHI commit: it is reviewed
  and merged here, never folded into the consumer's history.
- No AI or tool attribution in any commit message or pull request.

## Update policy

Refresh this file whenever a command, architecture contract or hard rule changes.
