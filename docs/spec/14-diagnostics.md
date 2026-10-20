# 14. Diagnostics

**Status**: Accepted
**API version**: 0.1.0

This chapter specifies four diagnostic channels: the validation tracker with its access
declarations, counters, scope timings and programmatic capture. The others live with the entries
they extend: every object carries a label ([fundamentals](01-fundamentals.md)), debug scopes from
`CommandList::beginScope` ([command recording](07-command-recording.md)) appear as debug groups in
captures, and messages leave through the single callback `setMessageCallback`
([fundamentals](01-fundamentals.md)), taking a severity and a message. What the device supports,
including upscalers, tile shapes and format support, is reported by `Device::capabilities`
([device](02-device.md)).

## Validation tracker

The validation tracker is the Validation tier. It and `CommandList::declareAccess` are compiled only
into non-Release (Debug) builds; a Release build contains neither, and `createDevice` with
`DeviceDesc::validation` set returns `ErrorCategory::Unsupported` there ([device](02-device.md)).
Within a non-Release (Debug) build the tracker is selected at run time by `DeviceDesc::validation`
and stays fixed for the device's lifetime. It is backend-neutral code shared by every backend, so a
violation is reported identically wherever it occurs. Rules only the tracker checks go unchecked
on a device created without it, and a violation of one is then undefined behaviour.

| Check | Rules it enforces |
|---|---|
| Every observable access and every barrier: a missing barrier, a stage or access that does not cover the use, a wrong old layout, a layout that does not permit the use | [synchronization](09-synchronization.md) |
| Cross-queue texture handoff, per queue | [synchronization](09-synchronization.md) |
| Cross-queue ordering: two uses of the same memory on two queues, at least one a write, with no fence between | [queues and frames](10-queues-and-frames.md) |
| Frame joins: compute and copy submissions of a frame that its graphics submissions do not wait on | [device](02-device.md) |
| Tile-mapping order: a mapping change and a use of its tiles on another queue with no fence between | [sparse resources](13-sparse.md) |
| Present layout: an image presented in a layout other than `Present` on the graphics queue | [presentation](11-presentation.md) |
| Lifetime: destroying a resource that recorded but unsubmitted work references | [device](02-device.md) |

The observable accesses are attachments, copy and clear endpoints, index reads, indirect-argument
reads, upscaler inputs and outputs, presented images, and the shader accesses declared through
`CommandList::declareAccess`. A shader access through a heap index or an `Addr` that is not
declared is untracked. The tracker checks necessity as well as sufficiency: an observable access
that happens to be ordered on one GPU without a barrier is still reported. Handle generations are
not the tracker's: a stale or destroyed resource handle is rejected by the Debug tier through
`ROJORHI_ASSERT`, with or without the tracker ([fundamentals](01-fundamentals.md)).

A violation is reported through the message callback at error severity, naming its valid-usage
identifier, and is then fatal like `ROJORHI_ASSERT`; a contract test asserts the identifier.

### `ResourceAccess`
```cpp
struct ResourceAccess {
    Buffer buffer = {}; uint64_t offset = 0, size = kWholeSize;
    Texture texture = {}; SubresourceRange range;
    Stage stage; Access access;
};
```
One shader access the next draw or dispatch makes through a heap index or an `Addr`, declared so
the tracker can check it. Stages and accesses are those of [synchronization](09-synchronization.md);
`SubresourceRange` is defined in [textures and heaps](04-textures-and-heaps.md).

| Field | Meaning |
|---|---|
| `buffer`, `offset`, `size` | Buffer and byte range accessed; `kWholeSize` ([synchronization](09-synchronization-part2.md)) runs to the end of the buffer |
| `texture`, `range` | Texture and subresources accessed |
| `stage` | Shader stage that makes the access |
| `access` | Shader read, shader write, or both |

#### Valid usage
- `VU-rojo-diag-0001` Exactly one of `buffer` and `texture` is not null.
- `VU-rojo-diag-0002` The declared range lies inside the resource: `offset` inside the buffer and
  `offset + size` at most its size unless `size` is `kWholeSize`, or `range` inside the texture's
  mip levels and layers.
- `VU-rojo-diag-0003` `stage` is exactly one of `Stage::VertexShader`, `TaskShader`, `MeshShader`,
  `FragmentShader` and `ComputeShader`, and `access` holds only `Access::ShaderRead` and
  `Access::ShaderWrite`.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Use | read by the tracker only | read by the tracker only | read by the tracker only |
| Cost | native: no native counterpart is needed | native: no native counterpart is needed | native: no native counterpart is needed |

### `CommandList::declareAccess`
```cpp
void CommandList::declareAccess(std::span<const ResourceAccess> accesses);
```
Declares shader accesses for the next draw, indexed draw, mesh-task draw or dispatch recorded on
the list, including their indirect forms; several calls before one command accumulate. The tracker
then checks those accesses like any observable access. With validation off the call returns at
once, and in a Release build it is an empty inline function, so declarations cost nothing there;
the consumer and the library are therefore built with the same `NDEBUG` setting.

#### Valid usage
- `VU-rojo-diag-0004` The command list is recording: acquired and not yet ended.
- `VU-rojo-diag-0005` The resource's usage permits the access: a buffer write needs
  `BufferUsage::ShaderWrite`, a texture read `TextureUsage::Sampled`, a texture write
  `TextureUsage::Storage`.
- `VU-rojo-diag-0006` Each declared `stage` is one the next command runs: `Stage::ComputeShader`
  for a dispatch, a graphics shader stage for a draw.
- `VU-rojo-diag-0007` A draw or dispatch follows the declaration before `CommandList::end`.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | none: recorded for the tracker | none: recorded for the tracker | none: recorded for the tracker |
| Cost | cheap: a tracker record, no native call | cheap: a tracker record, no native call | cheap: a tracker record, no native call |

## Counters and timings

### `Counters`
```cpp
struct Counters {
    uint64_t nativeCreateCalls = 0, nativeRecordCalls = 0, nativeSubmitCalls = 0;
    uint64_t nativeHostCalls = 0;
    uint64_t cpuAllocations = 0, gpuAllocations = 0;
    uint64_t frameDataUploads = 0, frameArenaBytes = 0, frameArenaGrowths = 0;
    uint64_t descriptorWrites = 0;
    uint64_t globalBarriers = 0, bufferBarriers = 0, textureBarriers = 0, nativeBarriers = 0;
    uint64_t pipelinesCreated = 0, pipelineCacheHits = 0, pipelineCacheMisses = 0;
    uint64_t pipelineCompileNanoseconds = 0;
    uint64_t encodersOpened = 0;
    uint64_t deferredDestructionsRetired = 0;
};
```
Plain integer counters, always compiled in, each maintained by a single increment. They count from
device creation and never reset; a caller measures an interval by subtracting two snapshots. They
are backend-neutral, so the measurement methodology scores them on every backend, and they replace
the Metal-specific frame-data counter the v1 conformance checkpoint flags as non-portable.

| Field | Counts |
|---|---|
| `nativeCreateCalls` | Native calls that create or release objects, views and memory |
| `nativeRecordCalls` | Native calls recorded into command buffers: state, draws, dispatches, copies, barriers |
| `nativeSubmitCalls` | Native queue calls: submission, presentation, tile mapping, fence signals and waits |
| `nativeHostCalls` | Other native calls on the CPU: residency commits, fence queries, timestamp reads |
| `cpuAllocations` | CPU heap allocations RojoRHI makes |
| `gpuAllocations` | Native memory objects created: buffers, textures, heaps and frame-arena pages |
| `frameDataUploads` | `CommandList::uploadFrameData` calls |
| `frameArenaBytes` | Bytes those calls copied, excluding alignment padding |
| `frameArenaGrowths` | Frame-arena growth events |
| `descriptorWrites` | Heap-slot writes: Metal 4 ID-table entries, descriptors on the planned D3D12 backend |
| `globalBarriers`, `bufferBarriers`, `textureBarriers` | Barrier records passed to `CommandList::barrier`, by kind |
| `nativeBarriers` | Native barrier commands emitted after batching |
| `pipelinesCreated` | Pipelines created, including any variant a backend creates on its own |
| `pipelineCacheHits`, `pipelineCacheMisses` | Pipeline creations served, or not, by the pipeline cache |
| `pipelineCompileNanoseconds` | CPU time spent in pipeline creation; the one timed field, not exact |
| `encodersOpened` | Native encoders on Metal 4; render passes begun elsewhere |
| `deferredDestructionsRetired` | Deferred destructions whose native release has run |

#### Valid usage
- None.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Source | shared code increments each counter at its cause: object and heap-slot creation and release, a submission's fence waits and signals, presentation, tile mapping and fence queries; it also folds in, at submission, the record, barrier and encoder counts a backend notes on a list. The backend adds each list's allocator, command buffer and argument tables, staging buffers, the commit of a submission's lists, the staged upload's commit, the frame-event signal, the queue-progress signal of a compute or copy submission with no lists or several signals, frame-event and fence waits, allocator resets, queue-progress reads, residency commits, counter heaps, timestamp reads and invalidations, and pipeline-cache hits | increments in shared code, except the native calls only the backend makes, which it counts at each native call site | increments in shared code, except the native calls only the backend makes, which it counts at each native call site |
| Cost | cheap: one CPU increment per event, no native call | cheap: one CPU increment per event, no native call | cheap: one CPU increment per event, no native call |

### `Device::counters`
```cpp
Counters Device::counters() const;
```
Returns a snapshot of the counters by value. Recording counters accumulate in each command list and
fold into the device when the list is submitted, so recording stays lock-free; creation counters
increment on the creating thread, and a list acquired and never submitted contributes none. The call
is free-threaded; a snapshot taken while other threads create objects or submit may miss their
latest increments.

#### Valid usage
- None.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | copy of the device's counters | copy of the device's counters | copy of the device's counters |
| Cost | cheap: a CPU copy, no native call | cheap: a CPU copy, no native call | cheap: a CPU copy, no native call |

### `ScopeTiming`
```cpp
struct ScopeTiming {
    std::string_view label;
    uint64_t frame = 0;
    QueueType queue = QueueType::Graphics;
    uint32_t depth = 0;
    double gpuMilliseconds = 0.0;
};
```
The GPU duration of one debug scope, measured by timestamps the backend writes at the scope's
boundaries; every render pass and every upscale forms its own scope.

| Field | Meaning |
|---|---|
| `label` | The scope's label, or the render pass's or upscale's, with the default label substituted for an empty one |
| `frame` | Number of the frame the scope was recorded in, counting `Device::beginFrame` calls from one |
| `queue` | Queue type of the command list that recorded the scope |
| `depth` | Nesting depth; zero for an outermost scope |
| `gpuMilliseconds` | GPU wall time between the scope's two timestamps |

Durations are wall time, not a sum of shader costs: nested scopes overlap their parent, and scopes
on different queues overlap each other. On a tile-based GPU a scope inside a render pass brackets
the fragment work finished at its boundaries, not its draws' share of the pass, and may report a
few microseconds inside a much longer pass; a render pass's own load and store work may fall partly
outside its duration.

#### Valid usage
- None.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Timestamps | `MTL4::CounterHeap`s of timestamps the device owns per frame slot: up to eight of 4,096 timestamps, each created when a scope first needs it, so 16,384 scopes a frame. A render pass writes its pair on its encoders, at `MTL4::TimestampGranularityPrecise`: the begin timestamp first on its first encoder, with `MTL4::ComputeCommandEncoder::writeTimestamp` on the encoder of its counted draws' kernels when they run first ([GPU-driven](08-gpu-driven.md)), so its time takes them in, else with `MTL4::RenderCommandEncoder::writeTimestamp` after `MTL::RenderStageVertex`; the end timestamp after `MTL::RenderStageFragment` on its last render encoder, which stays open after the pass. The encoders its splits resume lie between and write none, and the one encoder of a pass that records nothing, opened at `endRenderPass`, writes both. A pass's load, store and resolve work may fall partly outside its pair, so a pass that records nothing may report about zero. A scope inside a pass writes its pair on the render encoder open at each of its ends, after `MTL::RenderStageFragment` at the same granularity. `MTL4::CommandBuffer::writeTimestampIntoHeap` between encoders writes the pair of a scope outside a render pass and of an upscale (around its carrier encoder and MetalFX's encoders). A scope past that capacity, or whose heap Metal refuses (Metal allows 32 counter heaps per process or on the system; a refused heap is requested again after its frame slot's reuse), reports `gpuMilliseconds` 0, and `Device::beginFrame` logs one warning naming the frame | `D3D12_QUERY_TYPE_TIMESTAMP` queries | `VK_QUERY_TYPE_TIMESTAMP` queries |
| Conversion | GPU ticks, converted through `MTL::Device::queryTimestampFrequency`, read once at device creation; an entry Metal resolves as unwritten gives 0 | `GetTimestampFrequency` | `timestampPeriod` |
| Cost | cheap: a pair claimed with one atomic add and two timestamp writes; while Metal refuses a heap, one failed `MTL::Device::newCounterHeap` per frame for each refused heap the frame needs (more only when threads race to create it); outside a pass a scope boundary ends the open encoder, and a pass's and an in-pass scope's writes add no encoder. Measured on an Apple M3 Max (2026-10-19): a command buffer's `writeTimestampIntoHeap` costs about 1.5 µs of CPU and two allocations per write, and a pair of them around a render encoder made its draws about four times slower on the GPU (10,000 one-pixel draws in a pass); an encoder's `writeTimestamp`, precise or relaxed, costs neither measurably, so a pass's pair goes on its encoders. Moving the pair there took a frame of 10,000 such draws from 836 to 384 µs of GPU time (the native arm's 385) and a frame of one draw from 10.5 to 7.3 µs of CPU and from 21 to 18 allocations (Release, validation off, six processes each). The in-pass precise writes made no measurable change in a pass's duration (a pass of 64 draws with 0, 16 or 64 scopes inside it, two runs per case; the GPU clock varies between runs) | native | native |

### `Device::scopeTimings`
```cpp
std::span<const ScopeTiming> Device::scopeTimings();
```
Returns the scopes of the frame the most recent `Device::beginFrame` waited for, the frame
`framesInFlight` frames before the one it opened. Timestamps become readable only once their frame
retires, and that wait is the one point where retirement is already proven, so reading them never
stalls. Records are in submission order per queue, and within a command list in the order the
scopes began, a parent before its children. The span is empty until a frame has retired, and empty
for a frame with no scopes. It is owned by the device, labels included, and invalidated by the next
`Device::beginFrame`.

#### Valid usage
- None.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::CounterHeap::resolveCounterRange` once per heap the retired frame used, at `beginFrame`, then `invalidateCounterRange` over the used range | `ResolveQueryData` into a readback buffer, read at `beginFrame` | `vkGetQueryPoolResults` at `beginFrame` |
| Cost | native: timestamps resolved at the frame wait | native: timestamps resolved at the frame wait | native: timestamps resolved at the frame wait |

## Programmatic capture

### `Device::beginCapture`
```cpp
Result<void> Device::beginCapture(std::string_view path);
```
Starts a GPU capture of everything the device's queues execute until `Device::endCapture`, written
to the file `path`. The caller supplies the platform's extension: `.gputrace` on Metal 4, `.wpix` on
the planned D3D12 backend. The call returns an error when capture tooling is unavailable to the
process: on Metal 4 when capture is not enabled, or when the process-global `MTL::CaptureManager` is
already capturing for another device or tool; on the planned D3D12 backend when the device was
created without `DeviceDesc::enableCapture` ([device](02-device.md)), which loads the PIX GPU
capturer before the native device exists. It also returns an error when the destination cannot be
written. After an error no capture is in progress. RenderDoc is the Vulkan counterpart and is
reserved.

#### Valid usage
- `VU-rojo-diag-0008` No capture started by this device's `Device::beginCapture` is in progress.
- `VU-rojo-diag-0009` `path` is not empty.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::CaptureManager::startCapture` with an `MTL::CaptureDescriptor`, capturing the device to a GPU trace document | `PIXBeginCapture` with `PIX_CAPTURE_GPU` and the file name | reserved: RenderDoc `StartFrameCapture` with `SetCaptureFilePathTemplate` |
| Cost | native | native | native: RenderDoc in-application API |

### `Device::endCapture`
```cpp
void Device::endCapture();
```
Ends the capture `Device::beginCapture` started and finishes writing its file. Work submitted
before the call is in the capture.

#### Valid usage
- `VU-rojo-diag-0010` A capture started by this device's `Device::beginCapture` is in progress.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::CaptureManager::stopCapture` | `PIXEndCapture` | reserved: RenderDoc `EndFrameCapture` |
| Cost | native | native | native: RenderDoc in-application API |
