# RojoRHI

RojoRHI is a thin, explicit rendering hardware interface: an argument-table binding model, an
address-first per-frame data path, and an object model for resources, passes and pipelines. It
grows feature by feature, adding a capability only when a consumer has demonstrably needed it, and
takes no shader-toolchain, windowing or logging dependency of its own.

Metal 4, through metal-cpp, is its backend, and requires Apple Silicon macOS with the
`MTLGPUFamilyMetal4` device family; there is no Metal 3 fallback. An optional ImGui adapter builds
as a separate target.

Luminex is this library's required consumer: RojoRHI has no standalone application or sample
beyond its own conformance tests, and every change is driven by a Luminex branch through a RojoRHI
pull request before it reaches Luminex.

Public and licensed under the Apache License, Version 2.0; see [`LICENSE`](LICENSE). See
`AGENTS.md` for the golden sources, build commands and hard rules.
