# 07. Command recording: draws, dispatches and scopes

**Status**: Accepted
**API version**: 0.1.0

This part of the command-recording chapter ([command recording](07-command-recording.md)) defines
pipeline binding, direct draws and dispatches, and debug scopes. A shader finds its data through
the root block bound by `CommandList::setRoot` or `CommandList::pushRoot`
([shader ABI](05-shader-abi.md)); indirect forms are in [GPU-driven rendering](08-gpu-driven.md).
`rojoRHI::vertexIndex` includes the first vertex or base vertex, natively on Metal 4 and Vulkan and
provisionally on D3D12, where it is unverified, and `rojoRHI::instanceIndex` is zero-based
([built-ins](05-shader-abi-part3.md#built-ins)), so no base value travels through a root constant.
A draw or dispatch with a zero count records nothing.

### `CommandList::setPipeline`

```cpp
void CommandList::setPipeline(Pipeline pipeline);
```

Binds a graphics pipeline inside a render pass or a compute pipeline outside one. Binding a
graphics pipeline resets the topology to the list form of its class. `beginRenderPass` and
`endRenderPass` both unbind the pipeline, so a dispatch after a render pass binds its pipeline
again. On Metal 4 binding a compute pipeline opens the lazy compute encoder if none is open. A
render pass opens only on a `Graphics` list (`VU-rojo-cmd-0016`), so the list clause of
`VU-rojo-cmd-0021` follows from its pass clause.

#### Valid usage

- `VU-rojo-cmd-0020` `pipeline` is a live `Pipeline`.
- `VU-rojo-cmd-0021` A graphics pipeline is bound inside a render pass on a `Graphics` list, and
  its color formats, depth format and sample count equal those of the pass's attachments, in
  order.
- `VU-rojo-cmd-0022` A compute pipeline is bound outside a render pass on a `Graphics` or `Compute`
  list.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::RenderCommandEncoder::setRenderPipelineState` with the pipeline's depth-stencil and raster state, each set only where it differs from what the encoder holds (the winding only for a pipeline that culls, tests stencil or has a fragment entry); before the pass's render encoder opens, the binding and the dynamic state wait in the list and are set when it opens. Or `MTL4::ComputeCommandEncoder::setComputePipelineState`, set again at a dispatch when the encoder holds another | `SetPipelineState`, `IASetPrimitiveTopology` | `vkCmdBindPipeline` |
| Cost | cheap: depth-stencil object and raster state set where they differ | native | native |

### `CommandList::draw`

```cpp
void CommandList::draw(uint32_t vertices, uint32_t instances = 1, uint32_t firstVertex = 0,
                       uint32_t firstInstance = 0);
```

Draws `instances` instances of `vertices` vertices with the current topology. The shader pulls its
vertices; there is no vertex-input state.

#### Valid usage

- `VU-rojo-cmd-0023` Recorded inside a render pass with a graphics pipeline bound that has a vertex
  stage.
- `VU-rojo-cmd-0024` A viewport and a scissor have been set since `beginRenderPass`.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::RenderCommandEncoder::drawPrimitives` with a base instance of 0, which `rojoRHI::drawId` reads; `firstInstance` is invisible to shaders. The shortest form that says so: start and count for one instance, plus the instance count otherwise | `DrawInstanced` | `vkCmdDraw` |
| Cost | native | native | native |

### `IndexType`

```cpp
enum class IndexType : uint8_t { UInt16, UInt32 };
```

The width of each index in an index buffer: 16 or 32 bits.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::IndexTypeUInt16`, `MTL::IndexTypeUInt32` | `DXGI_FORMAT_R16_UINT`, `DXGI_FORMAT_R32_UINT` | `VK_INDEX_TYPE_UINT16`, `VK_INDEX_TYPE_UINT32` |
| Cost | native | native | native |

### `CommandList::drawIndexed`

```cpp
void CommandList::drawIndexed(Buffer indices, IndexType type, uint32_t count,
                              uint32_t instances = 1, uint32_t firstIndex = 0,
                              int32_t baseVertex = 0, uint32_t firstInstance = 0);
```

Draws `count` indices read from `indices` starting at index `firstIndex`, each added to
`baseVertex`. The index buffer is a per-draw argument because hardware index fetch and the
post-transform cache are worth keeping. Primitive restart is not part of 0.1.0, so with a strip
topology no index read may equal the largest value of `type`; the indices are GPU data, so no tier
diagnoses that, and the result of such an index is undefined.

#### Valid usage

- `VU-rojo-cmd-0025` Recorded inside a render pass with a graphics pipeline bound that has a vertex
  stage, and with a viewport and a scissor set since `beginRenderPass`.
- `VU-rojo-cmd-0026` `indices` was created with `BufferUsage::Index`, and
  `(firstIndex + count)` indices of `type` lie within it.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::RenderCommandEncoder::drawIndexedPrimitives`, index buffer by the GPU address and length read once at the buffer's creation, so the draw sends the buffer no message, with a base instance of 0 as for `draw`; the shortest form that says so: no instance count for one instance and base vertex 0, no base vertex for base vertex 0 | `IASetIndexBuffer` by GPU virtual address, `DrawIndexedInstanced` | `vkCmdBindIndexBuffer2`, `vkCmdDrawIndexed` |
| Cost | native | native | native |

### `CommandList::drawMeshTasks`

```cpp
void CommandList::drawMeshTasks(uint32_t x, uint32_t y, uint32_t z);
```

Launches an `x` by `y` by `z` grid of task threadgroups when the pipeline has a task stage, or of
mesh threadgroups when it does not. Threadgroup sizes come from the entry points' `[numthreads]`.

#### Valid usage

- `VU-rojo-cmd-0027` Recorded inside a render pass with a graphics pipeline bound that has a mesh
  stage, and with a viewport and a scissor set since `beginRenderPass`.
- `VU-rojo-cmd-0028` `x`, `y` and `z` are each at most `Limits::maxMeshTaskGroups`, and their
  product at most `Limits::maxMeshTaskGroupsTotal`
  ([limits](15-formats-and-limits-part2.md#limits)).

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::RenderCommandEncoder::drawMeshThreadgroups`, threadgroup sizes from the pipeline | `DispatchMesh` | `vkCmdDrawMeshTasksEXT` |
| Cost | native | native | native |

### `CommandList::dispatch`

```cpp
void CommandList::dispatch(uint32_t x, uint32_t y, uint32_t z);
```

Launches an `x` by `y` by `z` grid of threadgroups of the bound compute pipeline, whose size comes
from `[numthreads]`. On Metal 4 it records on the lazily opened compute encoder. Dispatches are not
ordered against each other or against copies without a barrier
([synchronization](09-synchronization.md)).

#### Valid usage

- `VU-rojo-cmd-0029` Recorded outside a render pass on a `Graphics` or `Compute` list.
- `VU-rojo-cmd-0030` A compute pipeline has been bound since the list began or since the last
  `endRenderPass`.
- `VU-rojo-cmd-0031` `x`, `y` and `z` are each at most `Limits::maxDispatchGroups`
  ([limits](15-formats-and-limits-part2.md#limits)).

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::ComputeCommandEncoder::dispatchThreadgroups` | `Dispatch` | `vkCmdDispatch` |
| Cost | native | native | native |

### `CommandList::beginScope`

```cpp
void CommandList::beginScope(std::string_view label);
```

Opens a scope: a debug group in captures and the first of a pair of GPU timestamps. Scopes nest, are
valid inside and outside render passes, and every render pass forms its own scope. The GPU time of
each scope is read through `Device::scopeTimings` ([diagnostics](14-diagnostics.md)), replacing
per-pass timings. The Metal 4 backend labels each encoder after its enclosing scope on a device
created with `validation` or `enableCapture`, or one whose class is not positively the bare driver's
(the validation layer). On a tile-based GPU, scopes nested inside one render pass may report coarse
times. Where a queue cannot write timestamps (some D3D12 copy queues, some Vulkan transfer
families), a scope on a `Copy` list is a debug group without timing.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | outside a render pass, `MTL4::CommandEncoder::endEncoding` on the open compute encoder, then `MTL4::CommandBuffer::pushDebugGroup` and `MTL4::CommandBuffer::writeTimestampIntoHeap` into the frame slot's counter heap ([diagnostics](14-diagnostics.md)); inside one, `MTL4::RenderCommandEncoder::pushDebugGroup` and `MTL4::RenderCommandEncoder::writeTimestamp` on the render encoder, which opens if it has not | `BeginEvent` and `EndQuery` with `D3D12_QUERY_TYPE_TIMESTAMP` | `vkCmdBeginDebugUtilsLabelEXT` and `vkCmdWriteTimestamp2` |
| Cost | cheap: one debug group and one timestamp, after the open encoder ends outside a pass, resolved once per frame | cheap: one query, resolved once per frame | cheap: one query, read once per frame |

### `CommandList::endScope`

```cpp
void CommandList::endScope();
```

Closes the innermost open scope and writes its second timestamp. The scope a render pass forms is
closed by `endRenderPass`, never by `endScope`, and is not one of the open scopes these rules count.
Since `endRenderPass` requires every scope begun inside the pass to have ended (`VU-rojo-cmd-0019`),
only the inside-a-pass half of `VU-rojo-cmd-0033` can be broken.

#### Valid usage

- `VU-rojo-cmd-0032` A scope is open on this list.
- `VU-rojo-cmd-0033` The innermost open scope began inside the current render pass when recorded
  inside one, and outside any render pass when recorded outside.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | outside a render pass, `MTL4::CommandEncoder::endEncoding` on the open compute encoder, then `MTL4::CommandBuffer::writeTimestampIntoHeap` and `MTL4::CommandBuffer::popDebugGroup`; inside one, `MTL4::RenderCommandEncoder::writeTimestamp` and `MTL4::RenderCommandEncoder::popDebugGroup` on the open render encoder | `EndEvent` and `EndQuery` | `vkCmdEndDebugUtilsLabelEXT` and `vkCmdWriteTimestamp2` |
| Cost | cheap: one timestamp and one debug group, after the open encoder ends outside a pass | cheap: one query | cheap: one query |
