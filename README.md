# RojoRHI

RojoRHI is a thin, explicit rendering hardware interface over the shared conceptual core of
Metal 4, D3D12 and Vulkan: an argument-table binding model, an address-first per-frame data path,
and an object model for resources, passes and pipelines. It grows feature by feature, adding a
capability only when a consumer has demonstrably needed it, and takes no shader-toolchain,
windowing or logging dependency of its own.

Metal 4, through metal-cpp, is its only backend, and requires Apple Silicon macOS with the
`MTLGPUFamilyMetal4` device family; there is no Metal 3 fallback. An optional ImGui adapter is
planned alongside the backend as a separate target.

Luminex is this library's required consumer: RojoRHI has no standalone application or sample
beyond its own conformance tests, and every change is driven by a Luminex branch through a
RojoRHI pull request before it reaches Luminex.

This repository currently holds its founding documents — conventions, architecture decisions and
an extraction guide — with no source, build or shader file yet; see `AGENTS.md` for the golden
sources and the planned commands once code arrives.

Private. All rights reserved.
