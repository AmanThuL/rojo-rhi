# 02. Device

**Status**: Accepted
**API version**: 0.1.0

The `Device` owns every resource, queue and frame slot RojoRHI creates. This chapter specifies its
creation and teardown, the capabilities it reports, the frame calls that pace the CPU against the
GPU, and deferred destruction. Queues and fences are specified in
[queues and frames](10-queues-and-frames.md), counters, scope timings and captures in
[diagnostics](14-diagnostics.md), and format support and limits in
[formats and limits](15-formats-and-limits.md) and its [part 2](15-formats-and-limits-part2.md).

## Frames and deferred work

A device keeps `DeviceDesc::framesInFlight` frame slots. Each slot owns the command allocators of
the command lists acquired during its frame, a frame arena for `CommandList::uploadFrameData`, and
the list of objects destroyed while it was open. `Device::beginFrame` waits until the slot last used
`framesInFlight` frames earlier has retired, then recycles all three; `Device::endFrame` signals the
slot's internal fence on the graphics queue. Within the frame loop the only implicit waits are frame
pacing in `Device::beginFrame` and `Swapchain::acquire`, which may block until a swapchain image, a
drawable on Metal 4, is free ([presentation](11-presentation.md)). Both concern only the graphics
queue: work on compute and copy queues is ordered by caller fences. Outside the frame loop, teardown
(`destroyDevice`) and presentation setup (`Device::createSwapchain`, `Swapchain::resize`, see
[presentation](11-presentation.md)) may also wait. The frame pacing model as a whole is described in
[queues and frames](10-queues-and-frames.md).

## Entries

### `DeviceDesc`
```cpp
struct DeviceDesc {
    bool validation = false;                    // run the validation tracker
    bool enableCapture = false;                 // prepare programmatic GPU capture before creation
    bool scopeTimings = false;                  // write GPU timestamps at scope boundaries
    uint32_t framesInFlight = 3;                // frame slots: 2 or 3
    std::span<const std::byte> pipelineCache;   // blob from Device::pipelineCacheData; empty for none
    std::string_view label;                     // "rojorhi.device.unnamed" when empty
};
```
- `validation` runs the validation tracker for the device's lifetime: every observable access and
  every barrier, cross-queue handoff and lifetime are checked, at a tracking cost, and a violated
  tracker-checked rule is reported and terminates the process. The tracker is compiled only into
  non-Release (Debug) builds, where it is selected per device at run time; a Release build contains
  no tracker, and `createDevice` with `validation` set returns `ErrorCategory::Unsupported` there.
  Timed measurements run with it off. The tracker's behaviour is specified in
  [diagnostics](14-diagnostics.md).
- `enableCapture` prepares the process for `Device::beginCapture` ([diagnostics](14-diagnostics.md))
  where the native capture tool must be loaded before the device exists. Without it, a capture
  request on such a backend fails as specified there.
- `scopeTimings` makes the device write GPU timestamps at the boundaries of every scope, render pass
  and upscale, read through `Device::scopeTimings` ([diagnostics](14-diagnostics.md)). Without it
  scopes, passes and upscales keep their debug groups and labels but write no timestamp, the device
  makes no timestamp storage and reads none at the frame wait, and `Device::scopeTimings` is empty.
- `framesInFlight` is the number of frame slots, which bounds how many frames the CPU records ahead
  of the GPU. Three, the default, keeps the GPU fed when CPU frame times vary; two lowers latency.
- `pipelineCache` is a blob the caller saved from `Device::pipelineCacheData` on an earlier device
  ([pipelines](06-pipelines.md)) and passes back so pipeline creation can hit instead of compile.
  It is read during `createDevice`. A blob from another driver, device or RojoRHI version is
  ignored, not an error.
- `label` names the device in captures and messages.

#### Valid usage
- `VU-rojo-device-0001` `framesInFlight` is 2 or 3.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| `validation` | backend-neutral tracker; independent of `MTL_DEBUG_LAYER` | backend-neutral tracker; independent of `ID3D12Debug::EnableDebugLayer` | backend-neutral tracker; independent of the validation layers |
| `enableCapture` | makes the encoders a list opens carry labels; capture availability is fixed at launch by `MTL_CAPTURE_ENABLED` and read through `MTL::CaptureManager::supportsDestination` | `LoadLibraryExW` of the newest `%ProgramFiles%\Microsoft PIX\<version>\WinPixGpuCapturer.dll` (the search `PIXLoadLatestWinPixGpuCapturerLibrary` makes) before the device factory; a machine without PIX creates the device and `Device::beginCapture` returns `Unsupported` | RenderDoc in-application API loaded before `vkCreateInstance` (reserved) |
| `scopeTimings` | the frame slots' `MTL4::CounterHeap`s, the timestamp writes and their resolve at the frame wait; without it none of them | each list's timestamp query heaps and readback buffers, the queries and their resolve at `end`; without it none of them | `VK_QUERY_TYPE_TIMESTAMP` query pools, the queries and their read at the frame wait |
| `pipelineCache` | a RojoRHI header around a Metal 4 archive: `MTL::Device::newArchive` from an `NS::URL`, looked up through `MTL4::CompilerTaskOptions::setLookupArchives`; saved with `MTL4::PipelineDataSetSerializer::serializeAsArchiveAndFlushToURL`. Both are URL-based, so a caller blob takes a temporary file, kept while the device lives | a RojoRHI header around an `ID3D12PipelineLibrary1`: `ID3D12Device1::CreatePipelineLibrary` on a copy of the library bytes, kept while the device lives | `VkPipelineCacheCreateInfo::pInitialData`, or `VK_KHR_pipeline_binary` data |
| Cost | costly: tracking when `validation` is set; native otherwise, `scopeTimings` adding its writes and resolves | costly: same | costly: same |

### `createDevice`
```cpp
Result<Device> createDevice(const DeviceDesc& desc = {});
```
Opens the system's GPU, checks the hardware floor, and creates the queues, the device-wide
residency or descriptor state, the frame slots and the message routing. It returns
`ErrorCategory::Unsupported` when `DeviceDesc::validation` is set in a Release build, which
contains no validation tracker, or when the floor is not met, naming the first missing requirement
in the message:

- Metal 4: macOS 26 or later and a GPU of the Apple7 family (M1) or newer, with read-write texture
  tier 2, BC texture compression, and at least the floor of each limit the device reports
  ([formats and limits, part 2](15-formats-and-limits-part2.md#limits)).
- D3D12: Windows 11, Agility SDK 1.619 or later, feature level 12_2, Shader Model 6.6
  with resource binding tier 3, `TypedUAVLoadAdditionalFormats`, enhanced barriers, and
  `DynamicDepthBiasSupported` for dynamic depth bias ([pipelines](06-pipelines-part2.md)).
  Per-vendor driver support for the last is unverified.

A device is not tied to a thread. Free-threaded; several devices may coexist.

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Open | `MTL::CreateSystemDefaultDevice` | `ID3D12SDKConfiguration1::CreateDeviceFactory` with the SDK beside the executable, `IDXGIFactory6::EnumAdapterByGpuPreference` (or `EnumWarpAdapter` on a WARP build), `ID3D12DeviceFactory::CreateDevice` at `D3D_FEATURE_LEVEL_12_2` | `vkCreateInstance`, `vkEnumeratePhysicalDevices`, `vkCreateDevice` |
| Floor check | `MTL::Device::supportsFamily` with `MTL::GPUFamilyMetal4` and `MTL::GPUFamilyApple7`; `MTL::Device::readWriteTextureSupport` at `MTL::ReadWriteTextureTier2`; `MTL::Device::maxArgumentBufferSamplerCount` at least 996, `MTL::Device::maxBufferLength` at least 2^32 (a `maxAddrOffset` of 2^32 − 1, above the floor's 2^29 − 1), `MTL::Device::maxThreadsPerThreadgroup` at least 1,024 x 1,024 x 64 and `MTL::Device::maxThreadgroupMemoryLength` at least 32,768, the floors of [formats and limits, part 2](15-formats-and-limits-part2.md#limits); `MTL::Device::supportsBCTextureCompression`, which the BC1 rows of [formats and limits](15-formats-and-limits.md) need | `ID3D12Device::CheckFeatureSupport` for `D3D12_FEATURE_SHADER_MODEL`, `D3D12_FEATURE_D3D12_OPTIONS` (binding tier, `TypedUAVLoadAdditionalFormats`, `ResourceHeapTier` 2, which `HeapDesc`'s `ALLOW_ALL_BUFFERS_AND_TEXTURES` needs (part 2 of memory and buffers), not a new floor); `maxViews` of at least 1,000,000 needs a view heap of 1,000,000 plus the reserved leading slots (2 + `framesInFlight` × 1,024), so a device whose `D3D12_FEATURE_D3D12_OPTIONS19` maximum is smaller is refused, and one without `OPTIONS19` is tried at that size (verified on WARP only), `D3D12_FEATURE_D3D12_OPTIONS12` (`EnhancedBarriersSupported`) and `D3D12_FEATURE_D3D12_OPTIONS16` (`DynamicDepthBiasSupported`) | `vkGetPhysicalDeviceFeatures2` for Vulkan 1.4, descriptor heap, device address commands and mesh shaders |
| Device state | `MTL::Device::newMTL4CommandQueue`, `MTL::Device::newResidencySet` added with `MTL4::CommandQueue::addResidencySet` | `ID3D12Device::CreateCommandQueue`, one shader-visible `ID3D12DescriptorHeap` per heap type, one global `ID3D12RootSignature` | `vkGetDeviceQueue`, `vkCreateCommandPool`, resource and sampler descriptor heaps |
| Cost | native | native | native |

### `destroyDevice`
```cpp
void destroyDevice(Device device);
```
Waits for all work submitted on the device's queues to complete, releases every object the device
still owns, and invalidates every handle and object handle it produced. With
`DeviceDesc::validation` set, each object still live is reported as a `Warning` message naming its
label. It is teardown, never part of a frame, and one of the waits outside the frame loop. Thread
class exclusive ([fundamentals](01-fundamentals.md)): a call on the device or any object it
produced that overlaps `destroyDevice` is undefined behaviour, which no tier detects.

#### Valid usage
- `VU-rojo-device-0002` No frame is open.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Teardown | `MTL::SharedEvent::waitUntilSignaledValue` per queue, then `release` | `ID3D12Fence::SetEventOnCompletion` per queue, then `Release` | `vkDeviceWaitIdle`, `vkDestroyDevice` |
| Cost | native | native | native |

### `Capabilities`
```cpp
struct Capabilities {
    std::string_view adapterName;             // the GPU's name as the native API reports it
    std::string_view floorName;               // the floor met, e.g. "Apple9" or "FL 12_2, SM 6.8"
    uint32_t maxViews = 0;                    // shader-visible texture views a shader can index
    uint32_t maxSamplers = 0;                 // shader-visible samplers a shader can index
    bool uploadInDeviceMemory = false;        // Memory::Upload is device-local and CPU-writable
    bool placementSparse = false;             // BufferUsage::Sparse and TextureUsage::Sparse work
    bool meshTasksIndirect = false;           // CommandList::drawMeshTasksIndirect works
    bool conservativeRaster = false;          // RasterState conservative rasterization works
    std::span<const UpscalerKind> upscalers;  // upscaler kinds Device::createUpscaler accepts
    Limits limits;                            // numeric limits; see formats and limits
};
```
A device's capabilities are fixed for its lifetime. Every string and span points at device-owned
storage valid until `destroyDevice`. Each `bool` gates the entries it names: using a gated feature
on a device that reports `false` is a valid-usage violation of the owning chapter.

- `adapterName` and `floorName` are for people: logs and bug reports. A caller branches only on
  the typed fields. `floorName` names the GPU family on Metal 4 and the feature level and highest
  shader model on D3D12.
- `maxViews` and `maxSamplers` bound the view and sampler heaps of
  [textures and heaps](04-textures-and-heaps.md).
- `uploadInDeviceMemory` reports whether `Memory::Upload` ([memory](03-memory-and-buffers.md))
  is GPU-local memory the CPU writes directly. It is always true on unified memory; where it is
  false, `Upload` is system memory the GPU reads across the bus.
- `placementSparse` reports whether reserved resources can be created and their tiles mapped
  ([sparse resources](13-sparse.md)); tile shapes come from `Device::tileShape`.
- `meshTasksIndirect` reports whether `CommandList::drawMeshTasksIndirect`
  ([GPU-driven rendering](08-gpu-driven.md)) is available. Direct mesh draws need no capability.
- `conservativeRaster` reports whether a pipeline may enable conservative rasterization
  ([pipelines](06-pipelines.md)).
- `upscalers` lists every `UpscalerKind` ([upscaling](12-upscaling.md)) other than `Auto` that
  creation would accept; it may be empty.
- `limits` holds the `Limits` defined in
  [formats and limits, part 2](15-formats-and-limits-part2.md#limits). Per-format support is
  queried through `Device::formatCapabilities` in [formats and limits](15-formats-and-limits.md).

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| `adapterName` | `MTL::Device::name` | `IDXGIAdapter1::GetDesc1` | `VkPhysicalDeviceProperties::deviceName` |
| `maxViews`, `maxSamplers` | views from the Metal feature set tables by GPU family; samplers from `MTL::Device::maxArgumentBufferSamplerCount`. Research figures of 1,000,000 textures and 996 (Apple7, Apple8) or 500,000 (Apple9 and later) samplers are unverified | resource binding tier 3: 1,000,000 views and 2,048 samplers; `D3D12_FEATURE_D3D12_OPTIONS19` where reported (unverified shipping values) | `VkPhysicalDeviceDescriptorHeapPropertiesEXT` (unverified structure name) |
| `uploadInDeviceMemory` | always true: unified memory | `D3D12_FEATURE_D3D12_OPTIONS16::GPUUploadHeapSupported` | a memory type with `DEVICE_LOCAL` and `HOST_VISIBLE` from `vkGetPhysicalDeviceMemoryProperties` |
| `placementSparse` | `MTL::Device::supportsPlacementSparse` | `D3D12_FEATURE_D3D12_OPTIONS::TiledResourcesTier`: tier 2 or higher; 3D textures at tier 3 | `VkPhysicalDeviceFeatures::sparseBinding` and the sparse residency features |
| `meshTasksIndirect` | `MTL::Device::supportsFamily` with `MTL::GPUFamilyApple9`, the family the feature tables list for indirect mesh draws (*unverified*) | always true at the floor: `ExecuteIndirect` with `D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH` | `VkPhysicalDeviceMeshShaderFeaturesEXT::meshShader` |
| `conservativeRaster` | always false: Metal 4 has no conservative rasterization | `D3D12_FEATURE_D3D12_OPTIONS::ConservativeRasterizationTier` | `VK_EXT_conservative_rasterization` |
| `upscalers` | `MTLFX::TemporalScalerDescriptor::supportsMetal4FX` | FSR when the FidelityFX runtime it loads is present beside the executable; DLSS and XeSS when built in and their runtime accepts the adapter | as D3D12: FSR, DLSS and XeSS have Vulkan variants |
| Cost | native: queried once at creation | native: queried once at creation | native: queried once at creation |

### `Device::capabilities`
```cpp
const Capabilities& Device::capabilities() const;
```
Returns the device's capabilities. The reference stays valid until `destroyDevice`. Free-threaded
and lock-free.

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | none: cached at creation | none: cached at creation | none: cached at creation |
| Cost | native | native | native |

### `Device::beginFrame`
```cpp
void Device::beginFrame();
```
Opens the next frame. The call waits until the frame slot last opened `framesInFlight` frames
earlier has retired on the graphics queue, then recycles that slot: its command allocators are
reset and its command lists return to the pool, its frame arena is rewound, and its deferred
destructions are released. What the retired frame publishes for measurement, through
`Device::counters` and `Device::scopeTimings`, is specified in [diagnostics](14-diagnostics.md).
The first `framesInFlight` calls do not wait. Thread class frame: a call that uses the open frame
racing `beginFrame` is undefined behaviour, which no tier detects.

#### Valid usage
- `VU-rojo-device-0003` No frame is open: every earlier `beginFrame` has been matched by an
  `endFrame`.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Wait | `MTL::SharedEvent::waitUntilSignaledValue` on the frame event | `ID3D12Fence::GetCompletedValue`, else `ID3D12Fence::SetEventOnCompletion` and `WaitForSingleObject` | `vkWaitSemaphores` on a timeline semaphore |
| Recycle | `MTL4::CommandAllocator::reset` | `ID3D12CommandAllocator::Reset` | `vkResetCommandPool` |
| Cost | native: one event wait and one allocator reset per slot | native: same | native: same |

### `Device::endFrame`
```cpp
void Device::endFrame();
```
Closes the open frame by signalling the slot's internal fence on the graphics queue after every
submission made to that queue during the frame. When the fence completes, every graphics-queue
command list of the frame has finished and the slot may be recycled. Presentation is separate:
see `Queue::present` in [presentation](11-presentation.md). Thread class: frame.

#### Valid usage
- `VU-rojo-device-0004` A frame is open.
- `VU-rojo-device-0005` Every command list acquired during the open frame has been submitted.
- `VU-rojo-device-0006` Every submission of the open frame to a compute or copy queue is ordered
  before the frame's graphics-queue signal by a caller fence: the graphics-queue submissions of the
  same frame, together, wait, directly or through other fences, on fence values signalled by or
  after each of them. Checked by the validation tracker.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::CommandQueue::signalEvent` with the frame event | `ID3D12CommandQueue::Signal` | `vkQueueSubmit2` with a `VkSemaphoreSubmitInfo` signal on a timeline semaphore |
| Cost | native: one queue signal | native: one queue signal | native: one queue signal |

### `Device::destroy`
```cpp
void Device::destroy(Buffer);
void Device::destroy(Texture);
void Device::destroy(Pipeline);
void Device::destroy(ShaderLibrary);
void Device::destroy(Heap);
void Device::destroy(Fence);
void Device::destroy(Upscaler);
void Device::destroy(ViewIndex);
void Device::destroy(SamplerIndex);
void Device::destroy(Swapchain);
```
Destroys an object or releases a heap slot, deferred. What becomes invalid at once depends on the
kind: a resource handle's slot generation advances, so any later use of the handle is a detectable
misuse; a `ViewIndex` or `SamplerIndex` is released into the deferred path and is not handed out
again until that path completes, so no unretired work can read a reused slot; a `Swapchain` is a
pointer wrapper, and using it after the call is undefined behaviour; an image it holds acquired is
released with it. The native object or descriptor slot is released when the frame slot that was
open at the call, or the next one opened if none is, retires; the caller never waits and never
tracks GPU progress for destruction.
Destroying a `Buffer` invalidates every `Addr` derived from it ([memory](03-memory-and-buffers.md)).
Destroying a sparse `Texture` or `Buffer` unmaps all its tiles at the call
([sparse resources](13-sparse.md)).
Caller-reserved index ranges are released through `Device::release` instead
([textures and heaps](04-textures-and-heaps.md)). Free-threaded.

#### Valid usage
- `VU-rojo-device-0007` The object is not referenced by a command list that has been recorded but
  not yet submitted. Checked by the validation tracker.
- `VU-rojo-device-0008` A `Heap` is destroyed only after every resource placed in it has been
  destroyed.
- `VU-rojo-device-0009` A `Heap` is destroyed only while no tile mapped from it remains mapped.
- `VU-rojo-device-0010` A `ViewIndex` or `SamplerIndex` passed to `destroy` was returned by
  `Device::createView` or `Device::createSampler`, not written into a range from
  `Device::reserveViews` or `Device::reserveSamplers`.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Release | `MTL::ResidencySet::removeAllocation`, then `release` on the object; the set is committed later, at the next submission or staged upload | `Release` on the `ID3D12Resource`, `ID3D12PipelineState`, `ID3D12Heap`, `ID3D12Fence` or `IDXGISwapChain3`; slot returned to the descriptor-heap free list | `vkDestroyBuffer`, `vkDestroyImage`, `vkDestroyImageView`, `vkDestroySampler`, `vkDestroyPipeline`, `vkFreeMemory`, `vkDestroySemaphore`, `vkDestroySwapchainKHR` |
| Cost | cheap: one deferred-release queue entry, released at slot retirement | cheap: same | cheap: same |
