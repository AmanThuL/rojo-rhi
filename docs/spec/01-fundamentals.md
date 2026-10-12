# 01. Fundamentals

**Status**: Accepted
**API version**: 0.1.0

This chapter defines what every other chapter assumes: the shapes of public type, how failure and
misuse are reported, the threading contract, handles, and the shared value types. Labels, the
message callback and the version macros continue in [part 2](01-fundamentals-part2.md). Notation
and cost classes follow [conventions](00-conventions.md).

## Public type shapes

Public headers name only the C++23 standard library and RojoRHI's own types; no backend, windowing
or third-party type appears in them. Object handles, resource handles, the shared value types and
descriptor structs are trivially copyable; `Error`, and a `Result` holding one, own a string and
are not.

- **Objects with behaviour** (`Device`, `Queue`, `CommandList`, `Swapchain`) wrap one backend
  pointer and carry non-virtual member functions; see `ObjectHandle`.
- **Resources** (`Buffer`, `Texture`, `Pipeline`, `ShaderLibrary`, `Heap`, `Fence`, `Upscaler`)
  are 64-bit generation-checked identifiers; see `ResourceHandle`. Texture views and samplers are
  32-bit heap indices, `ViewIndex` and `SamplerIndex`, specified with
  [textures and heaps](04-textures-and-heaps.md).
- **Descriptor structs** are plain aggregates whose fields carry defaults, so designated
  initializers name only what differs. A descriptor is read during the call and not retained.
- **Spans and strings** (`std::span`, `std::string_view`) passed to an entry are read during the
  call; an entry that keeps data copies it.

Exactly one backend is compiled per build and there are no virtual interfaces: each backend
defines the public member functions directly. RojoRHI loads precompiled shader libraries and never
compiles shaders at run time.

## Failure and misuse

Two kinds of failure exist, and they never mix.

- **Errors** are conditions a correct caller cannot rule out: hardware or driver refusal, an
  unavailable feature, exhausted memory, a shader library built for another ABI version. Only
  creation entries report them, by returning `Result<T>`.
- **Misuse** is a violated valid-usage rule. It is never reported as an error: when a tier checks
  the rule, it reports the identifier through the message callback and terminates the process.

Checks run in three tiers:

| Tier | Selected by | Cost | Checks |
|---|---|---|---|
| Release | build configuration | none | none: `ROJORHI_ASSERT` argument checks, the validation tracker and `CommandList::declareAccess` are compiled out |
| Debug | build configuration | argument checks | argument ranges, handle generations, scope rules, through `ROJORHI_ASSERT` |
| Validation | `DeviceDesc::validation` at run time, in non-Release (Debug) builds only | tracking | every observable access (attachments, copy and clear endpoints, index and indirect reads, upscaler and presented images, shader accesses declared through `CommandList::declareAccess`) and every barrier, heap-slot typing, cross-queue handoff, lifetime |

Release builds contain neither the tracker nor `CommandList::declareAccess`, so nothing is tracked
in release: `createDevice` with `DeviceDesc::validation` set returns `ErrorCategory::Unsupported`
there. Within non-Release (Debug) builds the tracker stays selectable per device at run time. A rule
no enabled tier checks is undefined behaviour when violated. Shader accesses through heap indices or
`Addr` that are not declared are invisible to the tracker. The tracker is backend-neutral, so one
missing barrier is reported identically on every backend; its behaviour and
`CommandList::declareAccess` are specified in [diagnostics](14-diagnostics.md).

## Threading contract

RojoRHI creates no threads. The caller owns all threading and decides which thread calls which
entry point; the classes below state what is safe. An entry's semantics name its class.

| Class | Rule | Entry points |
|---|---|---|
| Free-threaded | Any thread, concurrently with any other call except those the other classes exclude; may lock internally. | `createDevice`; every `Device::create*`, `Device::loadShaderLibrary`, `Device::pipelineCacheData`, `Device::destroy`; view and sampler slot calls (`reserveViews`, `writeView`, `reserveSamplers`, `writeSampler`, `release`); `Device::queue`, `Device::acquireCommandList`; queries (`capabilities`, `formatCapabilities`, `tileShape`, `address`, `cpuPointer`, `completedValue`, `counters`); `Device::wait`; `Device::beginCapture`, `Device::endCapture`; `setMessageCallback` |
| Per command list | One thread at a time per `CommandList`; different lists record in parallel. Takes no locks. | every `CommandList::` member, including `uploadFrameData` |
| Per queue | One thread at a time per `Queue`; submissions to one queue are serialized by the caller. | `Queue::submit`, `Queue::present`, `Queue::mapTiles` |
| Per swapchain | One thread at a time per `Swapchain`, ordered with the `Queue::present` that consumes it. | `Swapchain::acquire`, `Swapchain::resize` |
| Frame | One thread at a time, never concurrent with a call that uses the open frame. | `Device::beginFrame`, `Device::endFrame`, `Device::scopeTimings` |
| Exclusive | No other call on the device or its objects runs concurrently. | `destroyDevice` |

A `CommandList` may move between threads between calls; the caller provides the happens-before
ordering. A frame-class call racing a call that uses the open frame (`Device::acquireCommandList`,
a `CommandList` member, `Queue::submit`) is undefined behaviour; RojoRHI does not detect it. The
message callback may run on any thread, including threads the native API owns.

The reasons: only the caller knows its scheduling priorities; a thread pool inside the RHI would
oversubscribe cores beside the caller's job system and the drivers' compiler threads; D3D12
compiles pipelines on the calling thread and Metal 4 compiles out of process, so RHI-owned threads
add nothing; and a library without threads has deterministic tests. A caller-provided job callback,
`TaskSink`, is reserved for a future backend that needs internal parallelism; 0.1.0 has none.

## Entries

### `Result`
```cpp
template <class T>
using Result = std::expected<T, Error>;
```
Every creation entry returns `Result<T>`: either the created value or an `Error`. `Result<void>`
carries no value. An entry that returns an error has created nothing and leaked nothing, and a
later identical call may succeed. Non-creation entries do not return errors; their failures are
misuse.

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Native failure | `nullptr` return with `NS::Error**` out-parameter, e.g. `MTL::Device::newLibrary` | `HRESULT`, e.g. from `ID3D12Device::CreateCommittedResource` | `VkResult`, e.g. from `vkCreateBuffer` |
| Cost | native | native | native |

### `Error`
```cpp
struct Error {
    ErrorCategory category;   // stable failure category
    std::string message;      // human-readable context, including the native error text
};
```
`category` is the part a caller branches on; it is stable across versions. `message` is for people:
its text may change in any release and names the object's label and the native error where one
exists.

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Message source | `NS::Error::localizedDescription` | `HRESULT` code and `ID3D12InfoQueue` text when enabled | `VkResult` code and debug-utils text when enabled |
| Cost | native | native | native |

### `ErrorCategory`
```cpp
enum class ErrorCategory : uint8_t {
    Unsupported,        // the hardware, driver or build lacks a required feature
    OutOfMemory,        // a device or host allocation failed
    DriverFailure,      // the driver refused a well-formed request
    ShaderAbiMismatch,  // a shader library carries another ROJORHI_SHADER_ABI_VERSION
};
```
- `Unsupported`: the device floor is not met, or a requested optional feature (an upscaler kind,
  sparse resources, a format capability) is unavailable. `Capabilities` in
  [the device chapter](02-device.md) predicts it.
- `OutOfMemory`: device or host memory is exhausted. Retrying after freeing memory may succeed.
- `DriverFailure`: the native API rejected a request RojoRHI considers valid, for example a pipeline
  the driver fails to compile or a malformed shader library blob.
- `ShaderAbiMismatch`: `Device::loadShaderLibrary` was given a library compiled against another
  shader ABI version (see [shader ABI](05-shader-abi.md)).

Enumerators are only ever appended; an existing value keeps its meaning.

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| `Unsupported` | `MTL::Device::supportsFamily` false, or a feature query false | `ID3D12Device::CheckFeatureSupport` below the floor; `DXGI_ERROR_UNSUPPORTED` | `VK_ERROR_FEATURE_NOT_PRESENT`, `VK_ERROR_EXTENSION_NOT_PRESENT` |
| `OutOfMemory` | `nullptr` from a `new*` call | `E_OUTOFMEMORY` | `VK_ERROR_OUT_OF_DEVICE_MEMORY`, `VK_ERROR_OUT_OF_HOST_MEMORY` |
| `DriverFailure` | `NS::Error` from `MTL4::Compiler` or `newLibrary` | `E_INVALIDARG` or `E_FAIL` from creation | other `VkResult` error codes |
| Cost | native | native | native |

### `ResourceHandle`
```cpp
struct Buffer        { uint64_t bits = 0; friend bool operator==(Buffer, Buffer) = default; };
struct Texture       { uint64_t bits = 0; friend bool operator==(Texture, Texture) = default; };
struct Pipeline      { uint64_t bits = 0; friend bool operator==(Pipeline, Pipeline) = default; };
struct ShaderLibrary { uint64_t bits = 0; friend bool operator==(ShaderLibrary, ShaderLibrary) = default; };
struct Heap          { uint64_t bits = 0; friend bool operator==(Heap, Heap) = default; };
struct Fence         { uint64_t bits = 0; friend bool operator==(Fence, Fence) = default; };
struct Upscaler      { uint64_t bits = 0; friend bool operator==(Upscaler, Upscaler) = default; };
```
A resource handle is a 64-bit identifier made of a 32-bit slot index, a 24-bit generation and an
8-bit type tag. The bit positions are not part of the interface: a caller copies, compares, hashes
and stores `bits` but does not decode it. The all-zero value is the null handle; no live resource
has it. Destroying a resource advances its slot's generation, so a stale copy never names the slot's
next occupant, and non-Release (Debug) builds reject it instead of faulting non-deterministically.
After 2^24 reuses a slot is retired rather than wrapped.

A handle is meaningful only to the `Device` that created it and to the command lists and queues
obtained from that device. Passing it to another device is undefined behaviour: generations are per
device, so non-Release (Debug) builds cannot tell a foreign handle from a local one. Creation and
destruction are specified by each resource's chapter and by `Device::destroy` in
[the device chapter](02-device.md).

#### Valid usage
- `VU-rojo-core-0001` A resource handle is not null, unless the entry it is passed to accepts the
  null handle.
- `VU-rojo-core-0002` A non-null resource handle's generation matches the current generation of
  its slot.
- `VU-rojo-core-0003` A non-null resource handle's type tag matches its C++ handle type.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Native object behind the slot | `MTL::Buffer`, `MTL::Texture`, `MTL::RenderPipelineState` or `MTL::ComputePipelineState`, `MTL::Library`, `MTL::Heap`, `MTL::SharedEvent`, `MTL4FX::TemporalScaler` | `ID3D12Resource`, `ID3D12PipelineState`, DXIL blob, `ID3D12Heap`, `ID3D12Fence`, upscaler SDK context | `VkBuffer`, `VkImage`, `VkPipeline`, `VkShaderModule`, `VkDeviceMemory`, timeline `VkSemaphore`, upscaler SDK context |
| Cost | cheap: one slot-table lookup per use, plus a generation compare in Debug | cheap: same | cheap: same |

### `ObjectHandle`
```cpp
class Device       { public: /* members in their chapters */ private: struct DeviceImpl* m_impl = nullptr; };
class Queue        { public: /* members in their chapters */ private: struct QueueImpl* m_impl = nullptr; };
class CommandList  { public: /* members in their chapters */ private: struct CommandListImpl* m_impl = nullptr; };
class Swapchain    { public: /* members in their chapters */ private: struct SwapchainImpl* m_impl = nullptr; };
```
An object handle is a value wrapper around one pointer to a backend-defined type the public headers
only declare. Copies refer to the same object; a default-constructed handle refers to none. Member
functions are non-virtual and defined by the compiled backend, so with link-time optimization a
call inlines into the caller. The shape maps one-to-one onto a future C wrapper.

Each object has one owner and lifetime: a `Device` lives from `createDevice` to `destroyDevice`
([device](02-device.md)); a `Queue` belongs to its device and is never destroyed
([queues and frames](10-queues-and-frames.md)); a `CommandList` comes from a per-frame pool and
returns to it when its frame retires ([command recording](07-command-recording.md)); a `Swapchain`
lives from `Device::createSwapchain` ([presentation](11-presentation.md)) to `Device::destroy`.
An object handle carries no generation, so using one after its object is destroyed, or a
`CommandList` after its frame retired, is undefined behaviour that no tier detects.

#### Valid usage
- `VU-rojo-core-0004` An object handle used to call a member function or passed to an entry point
  is not default-constructed.
- `VU-rojo-core-0005` A `Queue`, `CommandList` or `Swapchain` is used only with the `Device` that
  produced it.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| `Device` | `MTL::Device` | `ID3D12Device10` (Agility SDK 1.619) | `VkDevice` |
| `Queue` | `MTL4::CommandQueue` | `ID3D12CommandQueue` | `VkQueue` |
| `CommandList` | `MTL4::CommandBuffer` with a per-frame `MTL4::CommandAllocator` | `ID3D12GraphicsCommandList9`, for `RSSetDepthBias`, with a per-frame `ID3D12CommandAllocator` | `VkCommandBuffer` from a per-frame `VkCommandPool` |
| `Swapchain` | `CA::MetalLayer` | `IDXGISwapChain4` | `VkSwapchainKHR` |
| Cost | native: one pointer indirection | native | native |

### `Extent2D`
```cpp
struct Extent2D { uint32_t width = 0, height = 0; };
```
A width and height in texels or pixels, as the using entry states. A zero default lets an entry
give `{}` a meaning of its own, such as "the whole input" in [upscaling](12-upscaling.md); entries
that need a non-empty extent say so in their valid usage.

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Type | `MTL::Size` with depth 1 | `UINT` width and height fields | `VkExtent2D` |
| Cost | native | native | native |

### `Extent3D`
```cpp
struct Extent3D { uint32_t width = 1, height = 1, depth = 1; };
```
A width, height and depth in texels: the size of mip 0 of a texture
([textures and heaps](04-textures-and-heaps.md)) or of a region. Each member defaults to one, so a
two-dimensional extent names only `width` and `height`.

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Type | `MTL::Size` | `D3D12_RESOURCE_DESC` `Width`, `Height`, `DepthOrArraySize`; `D3D12_BOX` extents | `VkExtent3D` |
| Cost | native | native | native |

### `Offset3D`
```cpp
struct Offset3D { uint32_t x = 0, y = 0, z = 0; };
```
A texel position inside one mip level, used with an `Extent3D` to name a region. Offsets are never
negative.

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Type | `MTL::Origin` | `D3D12_BOX` `left`, `top`, `front`; `CopyTextureRegion` `DstX`, `DstY`, `DstZ` | `VkOffset3D` |
| Cost | native | native | native |

### `Rect2D`
```cpp
struct Rect2D { uint32_t x = 0, y = 0, width = 0, height = 0; };
```
A rectangle in framebuffer pixels with its origin at the top-left: a render area or a scissor
rectangle ([command recording](07-command-recording.md), [pipelines](06-pipelines.md)).

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Type | `MTL::ScissorRect`; a render area is the `MTL4::RenderPassDescriptor` render-target size plus a scissor | `D3D12_RECT` | `VkRect2D` |
| Cost | native | native: right and bottom computed from width and height | native |
