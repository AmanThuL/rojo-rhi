# RojoRHI

RojoRHI is a thin, explicit rendering hardware interface. It is address-first and has no binding
slots: a shader reaches its data through one root block, the GPU addresses inside it and heap
indices, and the CPU side keeps thin handles only where the native APIs force them. It grows
feature by feature, adding a capability only when a consumer has demonstrably needed it, and
takes no shader-toolchain, windowing or logging dependency of its own.

Metal 4, through metal-cpp, is its backend, and requires Apple Silicon macOS with the
`MTLGPUFamilyMetal4` device family; there is no Metal 3 fallback. A backend-neutral ImGui adapter
builds as a separate target.

A D3D12 backend is also implemented and conformant (T1) on one GPU, an NVIDIA GeForce RTX 3080 Ti
on driver 621.14, with its overhead budgets not yet met and other vendors unverified.

Luminex is this library's required consumer: every change is driven by a Luminex branch through a
RojoRHI pull request before it reaches Luminex. [`Examples/`](Examples/README.md) holds small
standalone programs, from a first compute dispatch to GPU-driven culling, mesh shaders, async
compute and temporal upscaling, built with `xmake f --rojorhi_examples=y`.

Public and licensed under the Apache License, Version 2.0; see [`LICENSE`](LICENSE). See
`AGENTS.md` for the golden sources, build commands and hard rules.
