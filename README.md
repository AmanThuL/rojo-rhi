# RojoRHI

RojoRHI is a thin, explicit rendering hardware interface in dependency-free C++23. It is
address-first and has no binding slots: a shader reaches its data through one root block, the GPU
addresses inside it and heap indices, and the CPU side keeps thin handles only where the native
APIs force them. It grows feature by feature, adding a capability only when a consumer has
demonstrably needed it, and takes no shader-toolchain, windowing or logging dependency of its own.

Metal 4, through metal-cpp, is its backend on macOS, and requires Apple Silicon with the
`MTLGPUFamilyMetal4` device family; there is no Metal 3 fallback. A backend-neutral ImGui adapter
builds as a separate target, and a test-only Null backend implements the whole interface with a
validation tracker.

A D3D12 backend, selected on Windows with the `rojorhi_backend` option, is also implemented and
conformant (T1) on one GPU, an NVIDIA GeForce RTX 3080 Ti on driver 621.14, with its overhead
budgets not yet met and other vendors unverified.

## This repository

This repository carries releases only: one commit per version, titled `RojoRHI X.Y.Z`, with an
annotated tag `vX.Y.Z` and a GitHub release. Development happens in a private repository, and each
release is exported from it. Pin a tag, not a branch.

- [`CHANGELOG.md`](CHANGELOG.md) lists the changes in each version, with breaking changes marked.
- [`docs/spec/`](docs/spec/00-conventions.md) is the API reference, which states the semantics of
  the interface; enter it through its conventions chapter.
- [`Examples/`](Examples/README.md) holds small standalone programs, from a first compute dispatch
  to GPU-driven culling, mesh shaders, async compute and temporal upscaling.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) says how to report a bug; [`SECURITY.md`](SECURITY.md) says
  how to report a vulnerability privately.

The version follows Semantic Versioning for the source interface: from 1.0.0, a removal or a
narrowing needs a major version, and a new entry arrives in a minor version. No binary
compatibility is promised; consumers build RojoRHI from source.

## Building

RojoRHI builds with [xmake](https://xmake.io). On macOS:

```
xmake setup                  # fetches metal-cpp and Slang
xmake f -m debug -y          # the default backend on macOS, Metal 4
xmake                        # builds RojoRHI and the ImGui adapter
xmake test RojoRHITests/contract
```

The Metal 4 test entries need a real device and the Metal toolchain (`xcrun metal --version`),
and run under `MTL_DEBUG_LAYER=1`. The Null backend builds on every host:

```
xmake f -m debug -y --rojorhi_backend=null
xmake test RojoRHITests/contract
xmake test RojoRHITests/death
```

The examples fetch GLFW, so they are opt-in: `xmake f --rojorhi_examples=y`.

Luminex, the library's required consumer, mounts RojoRHI as a submodule and pins its release tags.

Licensed under the Apache License, Version 2.0; see [`LICENSE`](LICENSE).
