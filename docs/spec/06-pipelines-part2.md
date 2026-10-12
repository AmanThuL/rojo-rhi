# 06. Pipelines: creation and dynamic state

**Status**: Accepted
**API version**: 0.1.0

This part of the pipelines chapter ([pipelines](06-pipelines.md)) defines compute pipelines,
pipeline creation, the pipeline cache and the dynamic state set on the command list.

### `ComputePipelineDesc`

```cpp
struct ComputePipelineDesc { ShaderEntry compute; std::string_view label; };
```

Describes a compute pipeline. The threadgroup size comes from the entry's `[numthreads]` attribute;
it is not part of the descriptor or of `dispatch`.

#### Valid usage

- `VU-rojo-pipeline-0013` `compute` is present.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::ComputePipelineDescriptor` | `D3D12_COMPUTE_PIPELINE_STATE_DESC` with the global root signature | `VkComputePipelineCreateInfo` |
| Cost | native | native | native |

### `Device::createPipeline`

```cpp
Result<Pipeline> Device::createPipeline(const GraphicsPipelineDesc& desc);
Result<Pipeline> Device::createPipeline(const ComputePipelineDesc& desc);
```

Creates a pipeline synchronously: on return the pipeline is complete and binding it never compiles.
The call is free-threaded and RojoRHI creates no threads for it; a consumer creates pipelines in
parallel on its own threads. The device's `PipelineCache` is consulted first; a hit skips
compilation. Compiler failure, driver refusal and out-of-memory return an error; descriptor misuse
asserts. The label defaults to the `rojorhi.` prefix ([fundamentals](01-fundamentals.md)).
Destroyed through `Device::destroy`.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::Compiler::newRenderPipelineState` or `MTL4::Compiler::newComputePipelineState`, plus `MTL::Device::newDepthStencilState` | `CreatePipelineState`, through `LoadPipeline` and `StorePipeline` on the pipeline library | `vkCreateGraphicsPipelines` or `vkCreateComputePipelines` with the pipeline cache |
| Compiles on | out of process; the call waits | the calling thread | driver-defined |
| Cost | native | native | native |

### `PipelineCache`

```cpp
using PipelineCache = std::span<const std::byte>;   // DeviceDesc::pipelineCache
```

An opaque blob of compiled pipelines. The consumer saves the bytes returned by
`Device::pipelineCacheData` and passes them back through `DeviceDesc::pipelineCache`
([device](02-device.md)); an empty span means no cache. A blob from another backend, device,
driver or RojoRHI version is discarded as a cache miss and reported through the message callback;
it is never an error. The bytes are read during `createDevice`.

#### Valid usage

- `VU-rojo-pipeline-0014` The bytes passed through `DeviceDesc::pipelineCache` stay valid and
  unchanged until `createDevice` returns.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::Device::newArchive(NS::URL*)` and `MTL4::CompilerDescriptor::setLookupArchives`; archives load from a URL, so the blob round-trips through a file | `ID3D12Device1::CreatePipelineLibrary` from the blob | `vkCreatePipelineCache` with initial data, or `VK_KHR_pipeline_binary` |
| Cost | cheap: one file written and loaded at device creation; unverified | native | native |

### `Device::pipelineCacheData`

```cpp
Result<std::vector<std::byte>> Device::pipelineCacheData();
```

Returns the current pipeline cache as a blob to save and pass back through
`DeviceDesc::pipelineCache` at a later `createDevice`. The blob covers every pipeline created on
the device so far. The call is free-threaded and may run concurrently with `createPipeline`;
pipelines created concurrently may be missing from the blob. Failing to serialize returns an
error.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::PipelineDataSetSerializer` harvest serialized as an archive to a file, then read back | `ID3D12PipelineLibrary::GetSerializedSize` and `Serialize` | `vkGetPipelineCacheData`, or pipeline binaries |
| Cost | cheap: one file written and read; unverified | native | native |

## Dynamic state

Dynamic state is the state a command list sets inside a render pass rather than baking it into a
pipeline; the set is what Metal 4, D3D12 and Vulkan 1.4 all set on the command list. Depth-stencil
and blend state are deliberately absent: they stay in the pipeline because D3D12 bakes them into
the pipeline state object.

Dynamic state lives for one render pass ([command recording](07-command-recording.md)). At
`beginRenderPass` it starts as follows, and it persists across `setPipeline` except for topology:

| State | At `beginRenderPass` | Set by |
|---|---|---|
| Viewport | unset; a draw requires it | `setViewport` |
| Scissor | unset; a draw requires it | `setScissor` |
| Stencil reference | 0 | `setStencilReference` |
| Blend constants | 0, 0, 0, 0 | `setBlendConstants` |
| Topology | the list form of the pipeline's class, reset by every `setPipeline` | `setPrimitiveTopology` |
| Depth bias | 0, 0, 0 | `setDepthBias` |

D3D12 command-list state persists across render passes, so the planned backend writes the starting
values at `beginRenderPass`; Metal 4 starts each render encoder fresh and Vulkan 1.4 sets them as
dynamic state.

### `CommandList::setViewport`

```cpp
struct Viewport {
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;   // framebuffer pixels, top-left origin
    float minDepth = 0.0f, maxDepth = 1.0f;
};
void CommandList::setViewport(const Viewport& viewport);
```

Sets the single viewport. The origin is the top-left corner of the attachments and `y` grows
downwards; clip-space `+y` points up on every backend. Multiple viewports are not part of 0.1.0.

#### Valid usage

- `VU-rojo-pipeline-0015` Recorded inside a render pass.
- `VU-rojo-pipeline-0016` `width` and `height` are greater than zero.
- `VU-rojo-pipeline-0017` `minDepth` and `maxDepth` lie in `[0, 1]`.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::RenderCommandEncoder::setViewport` | `RSSetViewports` | `vkCmdSetViewport` with negative height, so clip space stays `+y` up |
| Cost | native | native | native |

### `CommandList::setScissor`

```cpp
void CommandList::setScissor(const Rect2D& rect);   // framebuffer pixels
```

Sets the single scissor rectangle; fragments outside it are discarded. `Rect2D` is defined in
[fundamentals](01-fundamentals.md#rect2d).

#### Valid usage

- `VU-rojo-pipeline-0018` Recorded inside a render pass.
- `VU-rojo-pipeline-0019` `rect` lies within the render pass's `area`.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::RenderCommandEncoder::setScissorRect` | `RSSetScissorRects` | `vkCmdSetScissor` |
| Cost | native | native | native |

### `CommandList::setStencilReference`

```cpp
void CommandList::setStencilReference(uint8_t reference);
```

Sets the stencil reference for both faces, used by stencil compares and by `StencilOp::Replace`.
Separate front and back references are not part of 0.1.0.

#### Valid usage

- `VU-rojo-pipeline-0020` Recorded inside a render pass.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::RenderCommandEncoder::setStencilReferenceValue` | `OMSetStencilRef` | `vkCmdSetStencilReference` with `VK_STENCIL_FACE_FRONT_AND_BACK` |
| Cost | native | native | native |

### `CommandList::setBlendConstants`

```cpp
void CommandList::setBlendConstants(float r, float g, float b, float a);
```

Sets the color read by `BlendFactor::Constant` and `BlendFactor::OneMinusConstant`.

#### Valid usage

- `VU-rojo-pipeline-0021` Recorded inside a render pass.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::RenderCommandEncoder::setBlendColor` | `OMSetBlendFactor` | `vkCmdSetBlendConstants` |
| Cost | native | native | native |

### `CommandList::setPrimitiveTopology`

```cpp
enum class PrimitiveTopology : uint8_t {
    PointList, LineList, LineStrip, TriangleList, TriangleStrip,
};
void CommandList::setPrimitiveTopology(PrimitiveTopology topology);
```

Sets the exact topology for the following draws of the bound vertex pipeline. `setPipeline` resets
it to the list form of the pipeline's `PrimitiveClass` (`PointList`, `LineList` or
`TriangleList`). Primitive restart is not part of 0.1.0 (see `CommandList::drawIndexed`).

#### Valid usage

- `VU-rojo-pipeline-0022` Recorded inside a render pass with a graphics pipeline bound that has a
  vertex stage.
- `VU-rojo-pipeline-0023` `topology` belongs to the bound pipeline's `PrimitiveClass`.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | primitive type argument of each `MTL4::RenderCommandEncoder` draw, kept by the backend | `IASetPrimitiveTopology` | `vkCmdSetPrimitiveTopology` |
| Cost | native | native | native |

### `CommandList::setDepthBias`

```cpp
void CommandList::setDepthBias(float constant, float slopeScaled, float clamp);
```

Sets the depth bias added to each fragment's depth: `constant` in units of the format's minimum
resolvable depth difference, plus `slopeScaled` times the polygon's maximum depth slope, limited in
magnitude by `clamp` when `clamp` is not zero. The values apply only while the bound pipeline has
`RasterState::depthBias` set, and they persist across `setPipeline` until the render pass ends.
The backend re-issues them after a pipeline change where native state does not carry them: Metal 4
has no enable bit, so its backend sets a zero bias for pipelines without `depthBias` and restores
the values afterwards; whether D3D12 keeps them across `SetPipelineState` is unverified, so the
planned backend re-issues them too.

On D3D12, dynamic depth-bias values need the pipeline flag
`D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS` and the `DynamicDepthBiasSupported` cap, which
`createDevice` checks as part of the floor ([device](02-device.md#createdevice)); per-vendor driver
support for that cap is unverified. Without it, dynamic values would mean
hidden pipeline variants, which the zero-variant budget forbids.

#### Valid usage

- `VU-rojo-pipeline-0024` Recorded inside a render pass.
- `VU-rojo-pipeline-0025` The bound graphics pipeline was created with `RasterState::depthBias`
  set.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::RenderCommandEncoder::setDepthBias` | `RSSetDepthBias` (`ID3D12GraphicsCommandList9`) | `vkCmdSetDepthBias` |
| Cost | cheap: values re-issued at a pipeline change | native: needs `DynamicDepthBiasSupported`; per-vendor support unverified | native |
