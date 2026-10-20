# The D3D12 Backend

**Status**: Accepted

This page describes the D3D12 backend as it stands. `createDevice` opens a device on the Agility
runtime and the backend owns the device-wide native state: queues, fences and frames, shader-visible
descriptor heaps, root and command signatures, memory, buffers, heaps, textures, views and
samplers, and the debug layer. Command lists are acquired, ended and given frame data, and
submission waits and signals fences; no list encodes a native command yet. Everything below is
"implemented, verified on WARP only, not on hardware": the backend is not a capability until a
hardware run says so ([ADR 0009](../decisions/0009-conformance-and-valid-usage.md),
[documentation](../conventions/documentation.md)). The API reference under `docs/spec/` stays the
authority on every entry's semantics; [the Null backend](null-backend.md) describes the shared core
this backend translates to. The pins, the runtime layout, the floor probe and the toolchain record
are in [the build record](d3d12-build.md).

| Chapter | State |
|---|---|
| 01 fundamentals, 02 device | implemented, `createDevice` and `destroyDevice`; `createUpscaler` returns `Unsupported` |
| 03 memory and buffers, 04 textures and heaps | implemented, with views and samplers |
| 15 formats and limits | implemented: the format table and the limits |
| 10 queues and frames (with the fences of 09) | implemented: fences, `submit`, frames; `Queue::present` and `Queue::mapTiles` abort |
| 07 command recording | `acquireCommandList`, `end` and `uploadFrameData` only; every other recording entry aborts |
| 06 pipelines | implemented: shader libraries, graphics, mesh and compute pipelines and the pipeline cache ([drawing](d3d12-drawing.md)) |
| 08 GPU-driven work, 11 presentation, 12 upscaling, 13 sparse | not implemented: entries return `Unsupported` or abort, and `createSwapchain` is `Unsupported` |
| 14 diagnostics | the core's counters and checks; no capture hook, so `beginCapture` is `Unsupported` |

An entry the backend does not implement logs its name and aborts in every build, or returns
`Unsupported` when it returns a `Result`. Each entry calls the core's check first and then
translates; the backend holds no `ROJORHI_VU`.

## Build options

- `rojorhi_backend=d3d12` selects the backend. It builds on Windows only; a configure on another
  platform fails naming the platform. The Windows default stays `null`, and the `interface` CI job
  asserts it ([ADR 0012](../decisions/0012-language-and-dispatch.md)).
- `rojorhi_d3d12_adapter` is `hardware` (the default: the first high-performance hardware adapter
  that meets the floor of [ADR 0010](../decisions/0010-hardware-floor.md)) or `warp` (the software
  rasterizer, for hosted CI, which has no GPU).
- `rojorhi_d3d12_debug` is `mode` (the default: the debug layer and DRED in Debug builds, nothing
  in Release; it defines `ROJORHI_D3D12_DEBUG_MODE`), `off`, `layer` (the debug layer and DRED) or
  `gbv` (the debug layer, GPU-based validation and DRED). The value is `mode` and not `auto`
  because xmake reads `auto` as unset.

Both are build configuration, not environment variables: RojoRHI reads and sets none.

## Agility loading

`D3D12Loader.cpp` opens a device in this order, and each device owns what it opens
(`d3d12::Loaded`: device factory, DXGI factory, adapter, device):

1. The OS check: `RtlGetVersion` must report build 22000 or later, whatever the manifest says;
   otherwise `Unsupported`.
2. The device factory: `ID3D12SDKConfiguration1::CreateDeviceFactory` with the SDK version of the
   headers (619) and the absolute path of the `D3D12\` directory beside the executable, passed as
   UTF-8. A consumer executable defines no `D3D12SDKVersion` or `D3D12SDKPath` export.
3. The debug features of the build, on that factory only and never process-wide: the debug layer,
   GPU-based validation for `gbv`, and DRED (breadcrumbs, breadcrumb contexts and page faults forced
   on) with the layer. A configuration interface the factory refuses fails creation.
4. `CreateDXGIFactory2` without the debug flag (`dxgidebug.dll` is not redistributable), then the
   adapters of the configured kind: `EnumWarpAdapter` on a WARP build, otherwise
   `EnumAdapterByGpuPreference` with high-performance preference, software adapters skipped.
5. For each candidate in order, `ID3D12DeviceFactory::CreateDevice` at `D3D_FEATURE_LEVEL_12_2`
   and `checkFloor`. The first to pass wins; if none does, `Unsupported` names the first missing
   item.

The floor items are Shader Model 6.6, resource binding tier 3, `TypedUAVLoadAdditionalFormats`,
resource heap tier 2 (which `HeapDesc`'s `ALLOW_ALL_BUFFERS_AND_TEXTURES` needs), enhanced
barriers and dynamic depth bias. The evidence that the factory path loads the app-local runtime is
in [the build record](d3d12-build.md#agility-loading-evidence). Several devices coexist because
none shares a factory or a debug configuration.

## Device objects and teardown

`DeviceImpl` (`D3D12Device.h`) declares its members in creation order: the core (`DeviceCore`),
`loaded`, the message router, the optional `features`, the three `QueueImpl` (one per `QueueType`),
the frame state, the descriptor heaps, the heap directory, the root signatures, the stager, the
pipeline cache, the per-slot list pools and the two name strings behind `Capabilities`.
`createDevice` validates the description in the core, calls `loadDevice`, registers the retire hooks
(buffer, heap, texture, view, fence, pipeline, shader library, frame slot, frame wait) and creates
the native state in that order. A failure part way runs the same teardown.

`destroyDevice` idles the queues, idles the stager, has the core pass every remaining record to the
retire hooks, releases the pipeline library and its bytes, releases the kept fences, deletes the
`DeviceImpl` (which releases every other native object in reverse order of declaration) and releases
the device last. Its reference count must then reach 0, which `ROJORHI_INTERNAL_ASSERT` checks in
Debug builds: a child object that outlives the device is a defect. The factory goes after it.

## Capabilities and formats

`checkFloor` runs at creation and `reportCapabilities` fills the report from the device
(`D3D12Capabilities.cpp`). The report is honest about what the backend does: `placementSparse` and
`conservativeRaster` are false, `upscalers` is empty, `meshTasksIndirect` is true,
`uploadInDeviceMemory` is `GPUUploadHeapSupported`, and `floorName` reads `FL 12_2, SM 6.<n>` with
the highest shader model. `maxViews` is the view heap size less the
reserved leading slots, `maxSamplers` is 2,048,
`maxAddrOffset` is 2^29 − 1, and `maxBufferSize` is the adapter's dedicated video memory (shared
system memory when that is 0), capped by one resource's virtual address bits.

The format table (`D3D12Formats.cpp`) starts from the core's ceiling and narrows it by what the
device reports; no format reports a sparse capability, and a failed query is an error, never a
narrower table. A texture's native format depends on its usage: a `Sampled` depth texture is
created in the typeless family of its depth format so its depth and stencil planes both get views;
a format with an sRGB counterpart is created in the typeless family of both unless the device
reports relaxed format casting, in which case it is created as itself and `castableFormats` lists
the formats its views may use. Any other texture is created as itself. A stencil view reads plane 1
through a component mapping into the first component, and a `Storage` view of an sRGB format uses
its linear counterpart because a typed UAV has no sRGB form.

## Memory

`Memory::Gpu` is a `DEFAULT` heap, `Readback` is `READBACK`, and `Upload` is `GPU_UPLOAD` when the
device reports it and `UPLOAD` otherwise (`D3D12Memory.cpp`). Buffers and textures are created
committed with `CreateCommittedResource3` (buffers in `D3D12_BARRIER_LAYOUT_UNDEFINED`), or placed
in a heap with `CreatePlacedResource2`. A `Heap` is one `ID3D12Heap` with
`ALLOW_ALL_BUFFERS_AND_TEXTURES`, aligned to 4 MiB for `Gpu` so multisample textures can be placed
and to 64 KiB otherwise, its size rounded up to that alignment. An `Upload` or `Readback` buffer
is mapped once at creation and stays mapped until it retires. The backend makes no residency
calls: the interface exposes no residency control ([memory and buffers](
../spec/03-memory-and-buffers.md#residency)), and every allocation stays resident for its life.

A buffer's `Width` is its size rounded up to a multiple of 4. Each buffer record carries a
`BufferPayload` (`D3D12Addr.h`): the resource, its GPU address, the mapped pointer and the
view-heap index of its raw views. The retire hook releases the raw views to the core's view
allocator, unmaps, releases the resource and frees the payload.

## Descriptor heaps

`DescriptorHeaps` (`D3D12Descriptors.cpp`) creates two shader-visible heaps, `rojorhi.views` and
`rojorhi.samplers`, and one CPU-only heap of twins. The view heap holds
`D3D12_FEATURE_D3D12_OPTIONS19`'s maximum capped at 2^21 descriptors, or 1,000,000 when the
device does not report it. The core reserves the leading slots (0 and 1 and the transient ring,
2 + `framesInFlight` × 1,024), so `maxViews` is the heap size less them. A `maxViews` of
1,000,000 needs a heap of 1,000,000 plus the reserved slots: a device whose maximum is smaller
is refused with `Unsupported` naming `maxViews`, and a device that does not report the maximum
is tried at 1,000,000 and refused too. This is verified on WARP only. The sampler heap
always holds 2,048. The core's view and sampler allocators hand out the slots, so a `ViewIndex` or
`SamplerIndex` is the heap index a shader reads.

Slot 0 holds a null raw SRV and slot 1 a null raw UAV for the device's life, so a load through the
zero `Addr` reads zeros and a store through it is discarded. The transient ring follows them: one
range of `framesInFlight` blocks of 1,024 slots, each with a twin in the CPU-only heap. A frame
slot claims its next transient with one atomic increment and no lock; claiming more than 1,024 in
one frame slot logs "at most 1,024 storage-texture clears per frame slot" and aborts in every
build, and the block is claimable again once the slot's frame has retired. The ring and the two
held slots are the reserved leading slots, so they do not count in `maxViews`.

A texture with `RenderTarget` or `DepthStencil` usage gets, at creation, a CPU-only heap with one
RTV or DSV per mip and layer (per mip and depth slice for a 3D texture), immutable for its life
and released with the texture. Recording reads it without a lock. A single-layer view at
layer 0 uses the non-array dimension and a later layer the array form of one layer, so a
non-array view of a later layer does not exist (issue #90).

## Addresses

An `Addr` is `{lo, hi}`: `lo` is the view-heap index of the buffer's raw SRV, whose raw UAV, when
the buffer has one, is at `lo + 1`, and `hi` is the byte offset. Only `D3D12Addr.cpp` encodes and
decodes (`encodeAddr`, `core::decodeAddr`, `operator+`). A raw view is `DXGI_FORMAT_R32_TYPELESS`
with the raw flag and at most 2^27 four-byte elements, so one view reaches the first 512 MiB of a
larger buffer and `maxAddrOffset` is 2^29 − 1; the core checks every offset against it. A buffer
above 512 MiB is created without error and `address` still reaches only that range. A `Readback`
buffer has no raw views.

The heap directory (`HeapDirectory`) finds the buffer of a raw SRV: a `core::NativeTable` the size
of the view heap, written under a leaf mutex by creation and by the retire hook, read with one
acquire load. An entry is published before the buffer's handle is returned and cleared before the
payload is freed. Frame-arena pages are `Upload` buffers with one raw SRV, and `uploadFrameData`
returns the address of the page payload's view index and the offset. A page larger than the
address reach is a core defect recorded in issue #89.

## Root and command signatures

`RootSignatures` (`D3D12RootSignature.cpp`) creates, once per device:

| Object | Content |
|---|---|
| `rojorhi.root` (global) | version 1.1, flags `CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED` and `SAMPLER_HEAP_DIRECTLY_INDEXED`; parameter 0 a root SRV `t0` space 0 `DATA_VOLATILE`, parameter 1 one 32-bit root constant `b1` |
| `rojorhi.root.kernel` | four 32-bit root constants `b0` and the heap-indexed flag |
| draw | arguments root constant of parameter 1 then `DRAW`, stride 20 |
| indexed draw | constant then `DRAW_INDEXED`, stride 24 |
| mesh | constant then `DISPATCH_MESH`, stride 16 |
| dispatch | `DISPATCH`, stride 12, no root signature |

The serialized global signature lives as long as the device and names every pipeline stream; no
list binds the command signatures yet.

## Initial data

`Stager` (`D3D12Stager.cpp`) copies initial data of `Gpu` buffers and textures. Its queue,
`rojorhi.upload`, is a `DIRECT` queue no `QueueType` names, so it never runs a caller's lists and
never signals a caller's fence. Each upload fills a committed `UPLOAD` staging buffer, records the
copy into the stager's one allocator and list, executes, signals the stager's fence and waits for
that value before returning, one upload at a time under a leaf mutex, so the data is complete
before any work submitted afterwards. A texture is created in `D3D12_BARRIER_LAYOUT_COPY_DEST`;
the stager repacks its rows to the footprint's 256-byte pitch and then moves every subresource to
the generic `D3D12_BARRIER_LAYOUT_SHADER_RESOURCE`, which direct and compute queues accept.

## Queues, fences and frames

Each `QueueType` has an `ID3D12CommandQueue` (`rojorhi.queue.graphics|compute|copy`) and a
progress fence only it signals. A caller's `Fence` is an `ID3D12Fence`. `Queue::submit`, under the
core's tracker lock, checks and replays the lists, issues one `Wait` per wait value and one
`Signal` per signal value, then, for a compute or copy submission that joins no frame (no lists,
or more than one signal), signals the queue's progress fence. A destroyed fence's native fence is
kept until every queue's progress passes the point it was destroyed at.

`Device::completedValue` reads `GetCompletedValue` directly. `Device::wait` goes through
`waitForFence` (`D3D12Common.cpp`): it reads `GetCompletedValue`, and when the value is not
reached and the timeout is not 0 it arms the calling thread's own auto-reset event with
`SetEventOnCompletion` and waits in slices of at most ten seconds. A removed device reports
`UINT64_MAX`; either entry reports the removal and aborts before that value can reach the core.
`endFrame` signals the frame fence from the graphics queue with the frame number; `beginFrame`
waits for the frame whose slot it reuses, and reusing a slot resets its list pools and transient
ring and releases the kept fences every queue has passed.

## Debug layer and device removal

When the build enables the layer, `MessageRouter` registers an `ID3D12InfoQueue1` callback that
forwards each message with its severity (info and message as `Info`, warning as `Warning`, error
and corruption as `Error`), on whatever thread the runtime raises it, and aborts after an error
or corruption message. Messages are dropped only by the deny list `kFiltered` in `D3D12Debug.cpp`,
which holds the runtime's per-object create and destroy notices at information severity and six
pipeline-library outcomes the cache reads from the HRESULT (not found, duplicate name, invalid
blob, driver or adapter mismatch, unsupported; see [drawing](d3d12-drawing.md)); message 972 passes.
Every other error, warning and application message passes, and so does a live-object report. On
`DXGI_ERROR_DEVICE_REMOVED`, `_RESET` or `_HUNG`, `d3d12::failure` reports the removal reason, the
auto-breadcrumb nodes and the page-fault data as `Error` messages, then the entry fails as its
contract says: a `Result` error, or a logged abort in an entry with no `Result`. Every native
object is named from its label.

## Threading and lock order

RojoRHI creates no thread. Locks the backend adds, outermost first: the list-pool mutex of a slot
and queue type (acquisition only); the frame-arena claim, then the core's view allocator, then the
directory's mutex. The heap directory mutex, the stager mutex, the kept-fence mutex and a list
pool's mutex are leaves. The tracker lock in `submit` is held from the check to the last native
call. Release recording reads the directory and each texture's target heap without a lock and
claims transient slots with an atomic. The pipeline cache's mutex is a leaf: creation holds it
only around `LoadPipeline` and `StorePipeline`, never while compiling, and `pipelineCacheData`
around `Serialize`; no pool lock is held when it is taken, and recording never takes it.

## Shader libraries and pipelines

[Drawing on the D3D12 backend](d3d12-drawing.md) describes the container, the loader, the
pipelines and the cache. The files are:

- `D3D12ShaderLibrary.{h,cpp}`: the `.d3d12lib` parser, the library payload and its retire hook.
- `D3D12PipelineState.{h,cpp}`: the pipeline payload, the state conversions and the state stream.
- `D3D12PipelineKey.{h,cpp}`: the canonical key, its hash and the cache blob header.
- `D3D12PipelineCache.{h,cpp}`: the device's pipeline library and its mutex.
- `D3D12Pipelines.cpp`: `loadShaderLibrary`, `createPipeline` and `pipelineCacheData`.

## Counting

Counters follow the native call. `descriptorWrites` counts each `Create*View` and
`CreateSampler` the backend issues (raw views, the two null views, RTVs and DSVs);
`nativeCreateCalls` counts each staging buffer and each internal object made after creation;
`cpuAllocations` counts each command list a pool grows by; `nativeSubmitCalls` counts each
`ExecuteCommandLists`, `Wait` and `Signal`; and `nativeHostCalls` counts each `GetCompletedValue`,
`SetEventOnCompletion` and `Map`. A wait for fence value 0 is not issued natively, since every
fence has reached 0 and the debug layer warns about one, but the core still counts it in
`nativeSubmitCalls`. `waitForFence` returns the number of host calls it made and its
caller counts them.

## The floor probe

Two probe executables under `Tools/D3D12Probe/` create a device on the app-local WARP at feature
level 12_2 and record the floor, the loaded modules and the debug-layer messages. On the hosted
runner both met the floor, loaded exactly the app-local `D3D12Core.dll` 1.619.6.0 and
`d3d10warp.dll` 1.0.21.0, and showed the layer, GPU-based validation and DRED active. The `d3d12`
job uploads their output as the artifact `d3d12-probe`. The full results are in
[the build record](d3d12-build.md#the-floor-probe); they are WARP results, not hardware evidence.

## Tests and CI

`Tests/D3D12/*.cpp` holds the backend's own cases, tagged `[d3d12]` (`[d3d12][death]` for death
cases). They may include the backend headers and reach native objects through
`DeviceImpl::of(device)`; native readbacks go through `Tests/Support/D3D12/`. The test target
globs the directory, so no build file lists them. The entries on a d3d12 build:

- `contract` runs `[contract]~[death]` and `[d3d12]~[death]`.
- `death` runs `[death]`.
- `conformance` runs `[conformance]` over the files the build does not hold back.

`d3d12_pending` in `xmake/targets.lua` lists the contract files of chapters the backend does not
implement yet, and `d3d12_conformance_pending` lists the conformance files it leaves out; both
hold one file name per line, a name that matches no file fails the configure, and a file leaves
the list when it passes whole on WARP under the debug layer and GPU-based validation.

The `d3d12` job on `windows-latest` runs a Debug configure with
`--rojorhi_d3d12_adapter=warp --rojorhi_d3d12_debug=gbv`, the probes and their gate, the
`RojoRHITests` build, the DXIL and runtime-DLL checks, `abi`, `contract`, `death` and
`conformance`, a case-count step with floors for `conformance`, `d3d12` and `contract`, the
toolchain record and the layout check (`--only Backends/D3D12 --only Tests/D3D12 --only
Tests/Support/D3D12 --only Tools/D3D12Probe`, with its own compilation database and `clangd`; the
macOS format job excludes the same directories). A Release configure with
`--rojorhi_d3d12_debug=mode` then builds warning-free under `/W4 /WX`, runs `contract` and
`conformance`, and builds the link-time-optimization probe for `check_lto_inline.py`. The Release
executables still turn off warnings C4702 and C4722 because entries not yet implemented abort
(issue #83).
