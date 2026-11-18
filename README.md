# RojoRHI

RojoRHI is an address-first rendering hardware interface in dependency-free C++23 over Metal 4
and Direct3D 12.

[![Build and GPU-free checks](https://img.shields.io/github/actions/workflow/status/AmanThuL/rojo-rhi/ci.yml?label=build%20and%20GPU-free%20checks)](https://github.com/AmanThuL/rojo-rhi/actions/workflows/ci.yml)
![Latest release](https://img.shields.io/github/v/release/AmanThuL/rojo-rhi)
![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue)
![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)
![Metal 4: conformance passing on 1 GPU](https://img.shields.io/badge/Metal%204-conformance%20passing%20(1%20GPU)-brightgreen)
![Direct3D 12: conformance passing on 1 GPU](https://img.shields.io/badge/Direct3D%2012-conformance%20passing%20(1%20GPU)-brightgreen)

## Why RojoRHI

- **Address-first with no binding slots.** Shaders reach buffers through GPU addresses and heap
  indices in one root block pushed per draw or dispatch. The Slang `RojoRHI` module owns that
  binding, so shaders declare none.
- **One backend per build, no virtual calls.** The backend is chosen at configure time, calls
  resolve at compile time, and link-time optimization inlines small calls such as
  `Device::capabilities`.
- **Near-native CPU cost.** On an Apple M4 Pro, a draw with a 64-byte root block costs 67.6 ns,
  against 66.0 ns in hand-written Metal 4 (+2.6%); [measured](docs/performance.md), not estimated.
- **Full Debug validation, zero Release cost.** Every valid-usage rule in the API reference has a
  runtime check and death test in Debug builds. Release builds leave them unchecked at no cost.
- **Dependency-free public headers.** Headers under `Include/` contain no platform, windowing, or
  third-party types.
- **No hidden threads or locks.** RojoRHI creates no threads, and command recording takes no
  locks in Release except when a command list claims a new frame-arena page.

## A first dispatch

This excerpt from the first compute example passes buffer GPU addresses in a root struct and
dispatches work without declaring any binding slots.

```cpp
struct SaxpyRoot
{
    Addr x;
    Addr y;
    Addr result;
    float a;
    uint32_t count;
};
// ...
Device device = check(createDevice({ .validation = example::kValidation, .label = "hello-compute" }), "createDevice");
// ...
Buffer xs = check(device.createBuffer({ .size = kBytes, .label = "x" }, std::as_bytes(std::span(x))), "createBuffer");
Buffer ys = check(device.createBuffer({ .size = kBytes, .label = "y" }, std::as_bytes(std::span(y))), "createBuffer");
Buffer result = check(device.createBuffer({ .size = kBytes, .usage = BufferUsage::ShaderWrite, .label = "result" }), "createBuffer");
// ...
device.beginFrame();
CommandList list = device.acquireCommandList(QueueType::Compute, "saxpy");
list.setPipeline(saxpy);
list.pushRoot(SaxpyRoot{ .x = device.address(xs), .y = device.address(ys), .result = device.address(result), .a = kA, .count = kCount });
list.dispatch((kCount + 63) / 64, 1, 1);
const std::array toCopy{ BufferBarrier{
    .buffer = result, .srcStage = Stage::ComputeShader, .srcAccess = Access::ShaderWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead } };
list.barrier({}, toCopy);
// ...
device.queue(QueueType::Compute).submit(std::array{ list }, {}, computeDone);
```

```slang
[shader("compute")]
[numthreads(64, 1, 1)]
void saxpy(uint3 thread : SV_DispatchThreadID)
{
    // Reads the block bound by pushRoot. The module owns that binding, so this shader declares none.
    SaxpyRoot root = rojoRHI::root<SaxpyRoot>();
    if (thread.x >= root.count)
    {
        return;
    }

    // load and store index elements of T past an Addr; only the module knows how each backend dereferences one.
    float x = rojoRHI::load<float>(root.x, thread.x);
    float y = rojoRHI::load<float>(root.y, thread.x);
    rojoRHI::store<float>(root.result, thread.x, root.a * x + y);
}
```

The shader reads the root block with `rojoRHI::root<SaxpyRoot>()` and indexes each buffer
through its `Addr`, with no binding declared anywhere. See
[`Examples/README.md`](Examples/README.md) for the complete program and further examples.

## Performance against native code

Each workload is written twice, once with RojoRHI and once directly in Metal 4 doing the same GPU
work, and both run on an Apple M4 Pro in Release builds with validation off. Budgets and statistics
are fixed before anything is timed; each configuration runs as 20 processes, each arm's figure is
the median of its process medians, and the difference is the median of the paired differences.

| Workload | RojoRHI | Native Metal 4 | Difference |
|---|---:|---:|---:|
| Per draw: a 64-byte root block and a draw, 1,000 a list | 67.6 ns | 66.0 ns | +2.6% |
| Per pipeline switch and draw, 100 a list | 119.4 ns | 116.0 ns | +3.7% |

RojoRHI itself makes no heap allocation per frame in any workload, and creates no pipeline or
native object after warm-up. [`docs/performance.md`](docs/performance.md) has every budget of each
backend's latest round, the ones still above budget included.

## Platforms

| Backend | OS | Minimum hardware | Tested on | Conformance | CPU cost against native |
|---|---|---|---|---|---|
| Metal 4 | macOS 26 or later | Apple silicon, GPU family Apple7 or newer | Apple M4 Pro, macOS 27.2.1 | passes, under Metal's validation layer | per operation within budget, 2.5 to 3.3% above native per draw in the three draw workloads; fixed cost per list above budget in two workloads and allocations in one; [full results](docs/performance.md) |
| Direct3D 12 | Windows 11 x64 | Feature level 12_2 (NVIDIA RTX 20 series, AMD RDNA 2, Intel Arc or newer) | NVIDIA GeForce RTX 3080 Ti, driver 621.14; other vendors' GPUs not tested | passes on that one GPU, under GPU-based validation | above the project's budgets when last measured; [full results](docs/performance.md) |

A test-only Null backend implements the full interface with the validation tracker and runs on
any host with no GPU. Minimum hardware is checked when a device is created, failing creation
below it with no reduced mode; see [the device specification](docs/spec/02-device.md).

## API features

What each backend offers, and which features a device reports for itself. Each backend's
verification status is under [Platforms](#platforms).

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

On any host, without a GPU, the Null backend builds and runs the contract suite:

```
xmake f -m debug -y --rojorhi_backend=null
xmake test RojoRHITests/contract
```

Examples fetch GLFW, so they are opt-in: `xmake f --rojorhi_examples=y`, then
`xmake run 01_HelloCompute`. See [`Examples/README.md`](Examples/README.md).

## Documentation

- [The API reference](docs/spec/00-conventions.md) specifies every entry, starting with
  conventions.
- [`Examples/`](Examples/README.md) holds small programs, from a first compute dispatch to
  GPU-driven culling, mesh shaders, async compute, and temporal upscaling.
- [`CHANGELOG.md`](CHANGELOG.md) records changes per release, with breaking changes marked.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) covers bug reports, and [`SECURITY.md`](SECURITY.md)
  covers private vulnerability reports.

## Versioning

This repository carries releases only: one commit per version, tagged `vX.Y.Z`; pin a tag, not a
branch. Versions follow Semantic Versioning for the source interface, with no binary compatibility;
build from source.

## Licence

Licensed under the Apache License, Version 2.0; see [`LICENSE`](LICENSE).
