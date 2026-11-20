# RojoRHI

RojoRHI is an address-first rendering hardware interface in dependency-free C++23, with native
Metal 4 and native Direct3D 12 behind one Slang shader ABI.

[![Build and GPU-free checks](https://img.shields.io/github/actions/workflow/status/AmanThuL/rojo-rhi/ci.yml?label=build%20and%20GPU-free%20checks)](https://github.com/AmanThuL/rojo-rhi/actions/workflows/ci.yml)
![Latest release](https://img.shields.io/github/v/release/AmanThuL/rojo-rhi)
![License: Apache-2.0](https://img.shields.io/badge/license-Apache--2.0-blue)
![C++23](https://img.shields.io/badge/C%2B%2B-23-blue)
![Metal 4: conformance passing on 1 GPU](https://img.shields.io/badge/Metal%204-conformance%20passing%20(1%20GPU)-brightgreen)
![Direct3D 12: conformance passing on 1 GPU](https://img.shields.io/badge/Direct3D%2012-conformance%20passing%20(1%20GPU)-brightgreen)

## What it is, and who it is for

RojoRHI is for teams writing a new GPU-driven renderer who write their own shaders in Slang. A
shader reaches its data through one root block per draw or dispatch, the GPU addresses inside it
and heap indices inside it. There are no binding slots, descriptor sets or pipeline layouts, and
one Slang module, `RojoRHI`, is the only code that knows how each backend lowers that model.

That combination is narrow, and when last surveyed it was empty: of the open-source rendering
hardware interfaces read on 2026-10-11, none combined Direct3D 12, Metal 4 and a pointer-first
root model. [NoGraphicsAPI](https://github.com/sebbbi/NoGraphicsAPI), the library by the author
of the "No Graphics API" proposal that RojoRHI's shader-facing model follows, has a native Metal 4
backend among others and no Direct3D 12 backend.

What it is not:

- **Not a backend for an existing engine.** It has no binding slots and no vertex-input state, so
  shaders written against slots or vertex layouts do not port onto it; every shader imports the
  `RojoRHI` module and pulls its own vertices.
- **Not a mobile or web interface.** The backends are Metal 4 on Apple silicon Macs and Direct3D 12
  on Windows 11.
- **Not a wide-compatibility layer.** Each backend has one hardware floor, checked when a device
  is created; below it creation fails, and there is no reduced mode.

## How a shader reaches its data

- **One root block, bound by address.** `CommandList::pushRoot(value)` allocates, copies and binds
  per-draw data from the frame's upload memory in one call. `CommandList::setRoot(Addr)` binds a
  root the caller already has, so a static root is built once and never re-uploaded.
- **`Addr`, an 8-byte value naming buffer memory.** On Metal 4 it is a GPU virtual address; on
  Direct3D 12 it is a raw-buffer heap index and a byte offset, because HLSL and DXIL have no
  pointers. Structs shared between C++ and Slang carry `Addr`, never a raw 64-bit pointer, and
  shaders read through it with `rojoRHI::load<T>` and `rojoRHI::store<T>`.
- **Heap indices.** `ViewIndex` and `SamplerIndex` are 32-bit indices into two global heaps;
  shaders fetch textures and samplers by index.
- **One definition of the built-ins.** `vertexIndex()`, `instanceIndex()` and `drawId()` mean the
  same on every backend.
- **Vertices are pulled by the shader**, and a pipeline holds only state a backend bakes at
  creation. Shaders are compiled offline; RojoRHI never compiles a shader at run time.

The cost of this model is published entry by entry. Dereferencing an `Addr` is native on Metal 4
and costly on Direct3D 12, one descriptor fetch per distinct base, so the API reference recommends
root fields and heap indices for hot per-draw data
([shader ABI, part 2](docs/spec/05-shader-abi-part2.md)). [The cost
summary](docs/spec/A-cost-summary.md) gives every entry's cost class on each backend.

## What the shader ABI checks for you

The same Slang source compiles to a Metal library and to DXIL. Every struct shared between C++ and
Slang is checked at build time against Slang reflection, and the build fails when the layouts
drift apart. The build writes the shader ABI version, `ROJORHI_SHADER_ABI_VERSION` (now 3), into
each compiled library, and `Device::loadShaderLibrary` refuses a library built for another version
with `ShaderAbiMismatch` instead of running a stale shader.

## Validation in Debug, nothing in Release

- **Misuse is fatal in Debug builds.** Every valid-usage rule in the API reference has a check and
  a death test. Release builds leave the checks out and pay nothing for them.
- **A backend-neutral tracker.** Selected with `DeviceDesc::validation` in Debug builds, it checks
  every barrier and observable access, cross-queue ordering and lifetimes, and reports a violation
  the same way on every backend. It checks necessity as well as sufficiency: a missing barrier is
  reported even on a GPU that happens to run correctly without it
  ([diagnostics](docs/spec/14-diagnostics.md)).
- **A Null backend.** Test-only, it implements the whole interface with the tracker and runs on
  any host with no GPU.

## A first dispatch

This excerpt from `01_HelloCompute` passes buffer addresses in a root block and dispatches with no
binding declared anywhere.

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
import RojoRHI;
// ... SaxpyRoot, declared with rojoRHI::Addr fields
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

[`Examples/README.md`](Examples/README.md) has the complete program and six more: a triangle with
no vertex layout, 256 textures in one instanced draw each picked by heap index, a compute pass
that culls 16,384 objects into counted indirect draws found through `drawId`, task and mesh
shaders with no vertex or index buffers, async compute ordered by two timeline fences, and
temporal upscaling. Debug builds run them under the tracker, so a missing or wrong barrier stops
the program with the valid-usage identifier it breaks.

## Performance against native code

Each workload is written twice, once with RojoRHI and once directly in Metal 4 doing the same GPU
work, and both run on an Apple M4 Pro under macOS 27.2.1 in Release builds with validation off.
Budgets and statistics are fixed before anything is timed; each configuration runs as 20
processes, each arm's figure is the median of its process medians, and the difference is the
median of the paired differences.

| Workload | RojoRHI | Native Metal 4 | Difference |
|---|---:|---:|---:|
| Per draw: a 64-byte root block and a draw, 1,000 a list | 67.79 ns | 65.99 ns | +2.64% |
| Per pipeline switch and draw, 100 a list | 119.79 ns | 116.04 ns | +3.23% |

The composed frame's GPU time, 2,130.39 µs against 2,145.70 µs, meets its budget of at most 1%
above native. RojoRHI itself makes no heap allocation per frame in any workload, and creates no
pipeline or native object after warm-up. The same round missed its budgets in three places, each
predicted with its cause before it was timed: a render pass at 10,000 a list, the fixed cost per
list in one workload, and the composed frame's allocations: it makes one more a frame than native
code, inside Metal, in a fence wait the barrier design keeps.
[`docs/performance.md`](docs/performance.md) has every budget of each backend's latest round, the
missed ones included.

Other properties that keep the cost visible: one backend per build, chosen when xmake configures,
with no virtual calls, and link-time optimization inlines small calls such as
`Device::capabilities`. RojoRHI creates no threads, and command recording takes no locks in
Release with native validation off, except when a command list claims a new frame-arena page.

## Platforms

| Backend | OS | Minimum hardware | Tested on | Conformance | CPU cost against native |
|---|---|---|---|---|---|
| Metal 4 | macOS 26 or later | Apple silicon, GPU family Apple7 or newer | Apple M4 Pro, macOS 27.2.1 | passes, under Metal's validation layer | per operation within budget but for a render pass at 10,000 a list; fixed cost per list above budget in one workload; one more allocation a frame than native, inside Metal; [full results](docs/performance.md) |
| Direct3D 12 | Windows 11 x64 | Feature level 12_2 (NVIDIA RTX 20 series, AMD RDNA 2, Intel Arc or newer) | NVIDIA GeForce RTX 3080 Ti, driver 621.14; other vendors' GPUs not tested | passes on that one GPU, under GPU-based validation | above the project's budgets when last measured; [full results](docs/performance.md) |

Minimum hardware is checked when a device is created, failing creation below it with no reduced
mode; see [the device specification](docs/spec/02-device.md). Public headers live under
`Include/rojoRHI/`, are included as `<rojoRHI/RHI.h>`, and name no platform, windowing or
third-party type. An optional ImGui adapter builds as a separate target.

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
  conventions; [the shader ABI](docs/spec/05-shader-abi.md) and [the cost
  summary](docs/spec/A-cost-summary.md) are the chapters to read first.
- [`Examples/`](Examples/README.md) holds small programs, from a first compute dispatch to
  GPU-driven culling, mesh shaders, async compute and temporal upscaling.
- [`CHANGELOG.md`](CHANGELOG.md) records changes per release, with breaking changes marked.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) covers bug reports, and [`SECURITY.md`](SECURITY.md)
  covers private vulnerability reports.

## Versioning

This repository carries releases only: one commit per version, tagged `vX.Y.Z`; pin a tag, not a
branch. Versions follow Semantic Versioning for the source interface, with no binary compatibility;
build from source.

## Licence

Licensed under the Apache License, Version 2.0; see [`LICENSE`](LICENSE).
