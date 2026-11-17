# RojoRHI

RojoRHI is a thin rendering hardware interface in dependency-free C++23 over Metal 4 and D3D12, and
[Supported platforms](#supported-platforms) gives the hardware each is tested on. It is
address-first: a shader reaches its data through one root block of GPU addresses and heap indices,
so there are no binding slots to declare. It is for engines and renderers that want the explicit
model of the native APIs without writing each backend themselves.

## Supported platforms

| Backend | OS | Minimum hardware | Tested on (GPU, driver, date) | Conformance | CPU overhead |
|---|---|---|---|---|---|
| Metal 4 | macOS 26 or later | Apple silicon, GPU family Apple7 or newer | Apple M4 Pro, macOS 27.2.1, 2026-11-17 | passes (T1) | overhead budgets not yet met (2026-11-17) |
| D3D12 | Windows 11 x64 | Feature level 12_2 (NVIDIA RTX 20, AMD RDNA 2, Intel Arc or newer) | NVIDIA GeForce RTX 3080 Ti, driver 621.14, 2026-11-17; other vendors unverified | conformant (T1) on one GPU | overhead budgets not yet met (2026-11-12) |

- **Conformance** "passes" means the conformance suite, which checks the results of every entry on
  the GPU under the platform's validation layer, passes unchanged on the tested hardware. A backend
  is verified only on the hardware the row names.
- **CPU overhead** is measured against hand-written native code doing the same work, to budgets
  fixed before the run: "met" or "not yet met".
- The minimum hardware is checked when a device is created: a device below it fails creation, and
  there is no reduced mode. The full list is in [the device chapter](docs/spec/02-device.md).
- A test-only **Null** backend implements the whole interface with a validation tracker and runs on
  any host, with no GPU. The `rojorhi_backend` option chooses the backend: `metal4` by default on
  macOS, `null` elsewhere.

## API features

What each backend offers, and which features a device reports for itself. Each backend's
verification status is under [Supported platforms](#supported-platforms).

| Feature | Metal 4 | D3D12 | Notes |
|---|---|---|---|
| Mesh and task shaders | yes | yes | Core: every device at the minimum hardware has them. |
| Indirect mesh draws | device: Apple9 or newer | yes | Optional, reported by `Capabilities::meshTasksIndirect`. |
| Indirect draws with a GPU count | yes, encoded by a backend kernel | yes | Core. |
| Draws per indirect call | 65,536 | 16,777,216 | A limit, `Limits::maxDrawIndirectCount`: the device's value, at least 65,535 everywhere. |
| Sparse tiles | device: every Apple7 Mac, Apple8 or newer | device: tiled resources tier 2 or higher; 3D textures need tier 3; no 1D | Optional, reported by `Capabilities::placementSparse`; 64 KiB tiles. |
| Upscalers | MetalFX | FSR, when its runtime sits beside the executable | Optional: `Capabilities::upscalers` lists the kinds a device offers, and an unlisted kind fails creation rather than being replaced. |
| Scope timings | yes | yes | Opt-in with `DeviceDesc::scopeTimings`: every scope, render pass and upscale is timed on the GPU. |
| Conservative rasterization | no | device: tier 1 or higher (tier 3 on the tested GPU) | Optional, reported by `Capabilities::conservativeRaster`; triangles only. |
| GPU capture | with `MTL_CAPTURE_ENABLED=1` | with `DeviceDesc::enableCapture` and PIX installed | Reported by `Capabilities::capture`; `beginCapture` returns `Unsupported` otherwise. |

**yes**: on every device at the minimum hardware. **no**: on no device. **device**: reported by
each device, true where the condition holds. A number is the value the backend reports. A feature
the interface does not name is not offered on any backend.

## Building

RojoRHI builds with [xmake](https://xmake.io). On macOS, with the Metal toolchain installed
(`xcrun metal --version`):

```
xmake setup                  # fetches metal-cpp and Slang
xmake f -m debug -y          # the default backend on macOS
xmake                        # builds RojoRHI and the ImGui adapter
xmake test RojoRHITests/contract
```

On Windows, from a Visual Studio developer command prompt:

```
xmake setup                  # fetches Slang, the Agility SDK, DXC and FidelityFX
xmake f -m debug -y --rojorhi_backend=d3d12
xmake
xmake test RojoRHITests/contract
```

The test entries need a real GPU, and Debug builds run under the platform's validation layer
(`MTL_DEBUG_LAYER=1` on macOS). The Null backend builds and tests on any host:

```
xmake f -m debug -y --rojorhi_backend=null
xmake test RojoRHITests/contract
```

The examples fetch GLFW, so they are opt-in: `xmake f --rojorhi_examples=y`, then
`xmake run 01_HelloCompute`. [`Examples/`](Examples/README.md) lists them.

## Documentation

- [The API reference](docs/spec/00-conventions.md) states the semantics of every entry; start at
  its conventions chapter.
- [`Examples/`](Examples/README.md) holds small programs, from a first compute dispatch to
  GPU-driven culling, mesh shaders, async compute and temporal upscaling.
- [`CHANGELOG.md`](CHANGELOG.md) lists each version's changes, breaking ones marked.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) says how to report a bug, and [`SECURITY.md`](SECURITY.md)
  how to report a vulnerability privately.

This repository carries releases only: one commit per version, tagged `vX.Y.Z`; pin a tag, not a
branch. The version follows Semantic Versioning for the source interface: a removal or a narrowing
needs a major version, and a new entry arrives in a minor version. No binary compatibility is
promised; build RojoRHI from source.

## Licence

Licensed under the Apache License, Version 2.0; see [`LICENSE`](LICENSE).
