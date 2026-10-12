# 08. GPU-driven rendering

**Status**: Proposed (until spike 3 of ADR 0014 reports)
**API version**: 0.1.0

GPU-driven work reads its arguments from GPU memory when it executes. A shader, usually a culling
pass, writes argument records and optionally a draw count; a barrier orders those writes before the
indirect command; the command then issues up to `maxCount` draws that all share the root block bound
by [`CommandList::setRoot`](05-shader-abi.md). Because one root serves the whole batch, every
indirect draw record begins with a `uint32` draw identifier, which the shader reads through
[`rojoRHI::drawId()`](05-shader-abi-part2.md) to find its own per-draw data.

Rules shared by every entry in this chapter:

- Arguments and counts are named by [`Addr`](03-memory-and-buffers.md), not by buffer handle:
  Metal 4 consumes them by GPU address, and a D3D12 address carries the heap index the backend
  resolves to a resource. The index buffer of an indexed draw is a `Buffer`, as for
  [`CommandList::drawIndexed`](07-command-recording.md).
- Records are tightly packed: record `i` starts at `args + i * sizeof(Record)`. Every field is a
  32-bit integer, so a record has the same natural layout on every target and needs 4-byte
  alignment.
- A null `count` means exactly `maxCount` draws. A non-null `count` names one `uint32` read when
  the command executes, and the command issues `min(count, maxCount)` draws.
- A GPU write on the same queue to the records or the count is ordered before the command by a
  barrier whose destination stage and destination access are both `IndirectArgs`
  ([synchronization](09-synchronization.md)). CPU writes are visible through submission, and
  writes on another queue are ordered by fences ([queues and frames](10-queues-and-frames.md)).
- Record contents and counts are written by the GPU and read at execution, so RojoRHI cannot
  validate them. A record whose values break the limits stated in its entry is undefined
  behaviour, not a valid-usage violation.
- Each indirect command follows the render-pass, pipeline and root rules of its direct form in
  [command recording](07-command-recording.md); only the source of its arguments differs.

Metal 4 has no multi-draw with a GPU count. Its backend runs a compute kernel that encodes an
indirect command buffer from the records and the count, ahead of the render pass that executes it,
and then executes that buffer with a GPU-written execution range. That path is `costly`: one extra
dispatch, a backend barrier between the kernel and the draws, and indirect-command-buffer memory
sized by `maxCount`. A single record with a null count is one plain indirect draw instead, which is
native. Spike 3, still pending, measures the encoding path against a `maxCount` loop of plain
indirect draws with an early out; this chapter stays `Proposed` until that spike reports.

## Argument records

### `DrawIndirectArgs`

```cpp
struct DrawIndirectArgs {        // 20 bytes, 4-byte aligned
    uint32_t drawId;
    uint32_t vertexCount;
    uint32_t instanceCount;
    uint32_t firstVertex;
    uint32_t firstInstance;
};
```

One non-indexed draw read by `CommandList::drawIndirect`.

- `drawId`: caller-defined value that `rojoRHI::drawId()` returns in every invocation of this draw.
- `vertexCount`: vertices per instance; zero makes the record a no-op.
- `instanceCount`: instances; zero makes the record a no-op.
- `firstVertex`: first vertex index; `rojoRHI::vertexIndex()` includes it, provisionally on D3D12
  ([unverified](05-shader-abi-part2.md#rojorhivertexindex)).
- `firstInstance`: first instance index. It is not visible to shaders:
  [`rojoRHI::instanceIndex()`](05-shader-abi-part2.md) is zero-based, and per-draw data comes
  through `drawId`.

`firstVertex + vertexCount` and `firstInstance + instanceCount` must not exceed 2^32 - 1.

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Draw fields | `MTL::DrawPrimitivesIndirectArguments` at record offset 4 | `D3D12_DRAW_ARGUMENTS` after a root-constant argument in the command signature | `VkDrawIndirectCommand` at record offset 4, stride 20 |
| `drawId` | the backend binds the record's address for each draw and the shader loads offset 0 | root constant read from record offset 0 | the backend binds the records' base address and stride, and the shader loads offset 0 of record `DrawIndex` |
| Cost | cheap: one load for `drawId` | native | cheap: one load for `drawId` |

### `DrawIndexedIndirectArgs`

```cpp
struct DrawIndexedIndirectArgs { // 24 bytes, 4-byte aligned
    uint32_t drawId;
    uint32_t indexCount;
    uint32_t instanceCount;
    uint32_t firstIndex;
    int32_t  baseVertex;
    uint32_t firstInstance;
};
```

One indexed draw read by `CommandList::drawIndexedIndirect`.

- `drawId`: caller-defined value that `rojoRHI::drawId()` returns in every invocation of this draw.
- `indexCount`: indices per instance; zero makes the record a no-op.
- `instanceCount`: instances; zero makes the record a no-op.
- `firstIndex`: first element read from the index buffer.
- `baseVertex`: signed value added to each fetched index; `rojoRHI::vertexIndex()` includes it,
  provisionally on D3D12 ([unverified](05-shader-abi-part2.md#rojorhivertexindex)).
- `firstInstance`: first instance index. It is not visible to shaders:
  [`rojoRHI::instanceIndex()`](05-shader-abi-part2.md) is zero-based, and per-draw data comes
  through `drawId`.

`firstIndex + indexCount` elements of the draw's index type must fit in the index buffer, and
`firstInstance + instanceCount` must not exceed 2^32 - 1.

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Draw fields | `MTL::DrawIndexedPrimitivesIndirectArguments` at record offset 4 | `D3D12_DRAW_INDEXED_ARGUMENTS` after a root-constant argument | `VkDrawIndexedIndirectCommand` at record offset 4, stride 24 |
| `drawId` | the backend binds the record's address for each draw and the shader loads offset 0 | root constant read from record offset 0 | the backend binds the records' base address and stride, and the shader loads offset 0 of record `DrawIndex` |
| Cost | cheap: one load for `drawId` | native | cheap: one load for `drawId` |

### `DrawMeshTasksIndirectArgs`

```cpp
struct DrawMeshTasksIndirectArgs { // 16 bytes, 4-byte aligned
    uint32_t drawId;
    uint32_t x, y, z;
};
```

One mesh-task draw read by `CommandList::drawMeshTasksIndirect`.

- `drawId`: caller-defined value that `rojoRHI::drawId()` returns in every invocation of this draw.
- `x`, `y`, `z`: threadgroup counts of the first stage, the task shader when the pipeline has one
  and the mesh shader otherwise, as for
  [`CommandList::drawMeshTasks`](07-command-recording.md); any zero makes the record a no-op.

Each of `x`, `y` and `z` must be at most `Limits::maxMeshTaskGroups`, and their product at most
`Limits::maxMeshTaskGroupsTotal` ([limits](15-formats-and-limits-part2.md#limits)).

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Draw fields | `MTL::DispatchThreadgroupsIndirectArguments` at record offset 4 | `D3D12_DISPATCH_MESH_ARGUMENTS` after a root-constant argument | `VkDrawMeshTasksIndirectCommandEXT` at record offset 4, stride 16 |
| `drawId` | the backend binds the record's address for each draw and the shader loads offset 0 | root constant read from record offset 0 | the backend binds the records' base address and stride, and the shader loads offset 0 of record `DrawIndex` |
| Cost | cheap: one load for `drawId` | native | cheap: one load for `drawId` |

### `DispatchIndirectArgs`

```cpp
struct DispatchIndirectArgs {    // 12 bytes, 4-byte aligned
    uint32_t x, y, z;
};
```

One compute dispatch read by `CommandList::dispatchIndirect`. It has no draw identifier, because
a dispatch is never batched. `x`, `y` and `z` are threadgroup counts per dimension; any zero makes
the dispatch a no-op. Each must be at most `Limits::maxDispatchGroups`
([limits](15-formats-and-limits-part2.md#limits)).

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Fields | `MTL::DispatchThreadgroupsIndirectArguments` | `D3D12_DISPATCH_ARGUMENTS` | `VkDispatchIndirectCommand` |
| Cost | native | native | native |

## Commands

### `CommandList::drawIndirect`

```cpp
void CommandList::drawIndirect(Addr args, uint32_t maxCount, Addr count = {});
```

Issues up to `maxCount` non-indexed draws whose `DrawIndirectArgs` records start at `args`, with
the bound pipeline and root. Draws execute in record order.

#### Valid usage
- `VU-rojo-indirect-0001` Recorded inside a render pass with a graphics pipeline that has a vertex
  shader bound.
- `VU-rojo-indirect-0002` `args` is non-null and 4-byte aligned, and `maxCount` records from
  `args` lie in one buffer created with `BufferUsage::IndirectArgs`.
- `VU-rojo-indirect-0003` `maxCount` is greater than zero and at most
  `Limits::maxDrawIndirectCount` ([limits](15-formats-and-limits-part2.md#limits)).
- `VU-rojo-indirect-0004` A non-null `count` is 4-byte aligned and lies in a buffer created with
  `BufferUsage::IndirectArgs`.
- `VU-rojo-indirect-0005` Every GPU write on the same queue to the records or the count is ordered
  before this command by a barrier whose destination stage and access include `IndirectArgs`.
  Checked by the validation tracker.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::RenderCommandEncoder::drawPrimitives` with an indirect buffer address when `maxCount` is 1 and `count` is null; otherwise a backend kernel encodes an indirect command buffer that `MTL4::RenderCommandEncoder::executeCommandsInBuffer` runs | `ExecuteIndirect` with a root constant and `D3D12_INDIRECT_ARGUMENT_TYPE_DRAW` in the command signature; `count` as the count buffer; the resource and offset are looked up from each `Addr`'s heap index | the device-address forms of `vkCmdDrawIndirect` and `vkCmdDrawIndirectCount` from `VK_KHR_device_address_commands`, which the oracle assumes ([ADR 0010](../decisions/0010-hardware-floor.md)) (entry-point names unverified) |
| Cost | costly: one extra dispatch and indirect-command-buffer memory; native when `maxCount` is 1 and `count` is null; spike 3 (pending) | cheap: index→resource lookup for `args` and `count` | native: entry points unverified |

### `CommandList::drawIndexedIndirect`

```cpp
void CommandList::drawIndexedIndirect(Buffer indices, IndexType type, Addr args, uint32_t maxCount,
                                      Addr count = {});
```

Issues up to `maxCount` indexed draws whose `DrawIndexedIndirectArgs` records start at `args`,
fetching indices of `type` ([`IndexType`](07-command-recording.md)) from `indices`. Draws execute
in record order.

#### Valid usage
- `VU-rojo-indirect-0006` Recorded inside a render pass with a graphics pipeline that has a vertex
  shader bound.
- `VU-rojo-indirect-0007` `indices` was created with `BufferUsage::Index`.
- `VU-rojo-indirect-0008` `args` is non-null and 4-byte aligned, and `maxCount` records from
  `args` lie in one buffer created with `BufferUsage::IndirectArgs`.
- `VU-rojo-indirect-0009` `maxCount` is greater than zero and at most
  `Limits::maxDrawIndirectCount` ([limits](15-formats-and-limits-part2.md#limits)).
- `VU-rojo-indirect-0010` A non-null `count` is 4-byte aligned and lies in a buffer created with
  `BufferUsage::IndirectArgs`.
- `VU-rojo-indirect-0011` Every GPU write on the same queue to the records or the count is ordered
  before this command by a barrier whose destination stage and access include `IndirectArgs`.
  Checked by the validation tracker.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::RenderCommandEncoder::drawIndexedPrimitives` with an indirect buffer address when `maxCount` is 1 and `count` is null; otherwise a kernel-encoded indirect command buffer | `IASetIndexBuffer`, then `ExecuteIndirect` with a root constant and `D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED`; the resource and offset are looked up from each `Addr`'s heap index | `vkCmdBindIndexBuffer2`, then the device-address forms of `vkCmdDrawIndexedIndirect` and `vkCmdDrawIndexedIndirectCount` from `VK_KHR_device_address_commands` (entry-point names unverified) |
| Cost | costly: one extra dispatch and indirect-command-buffer memory; native when `maxCount` is 1 and `count` is null; spike 3 (pending) | cheap: index→resource lookup for `args` and `count` | native: entry points unverified |

### `CommandList::drawMeshTasksIndirect`

```cpp
void CommandList::drawMeshTasksIndirect(Addr args, uint32_t maxCount, Addr count = {});
```

Issues up to `maxCount` mesh-task draws whose `DrawMeshTasksIndirectArgs` records start at `args`.
Draws execute in record order. Metal's feature tables list indirect mesh draws from the Apple9
family, above the Apple7 floor (*unverified*), so support is reported per device.

#### Valid usage
- `VU-rojo-indirect-0012` Recorded inside a render pass with a graphics pipeline that has a mesh
  shader bound.
- `VU-rojo-indirect-0013` The device reports `Capabilities::meshTasksIndirect`
  ([device](02-device.md)).
- `VU-rojo-indirect-0014` `args` is non-null and 4-byte aligned, and `maxCount` records from
  `args` lie in one buffer created with `BufferUsage::IndirectArgs`.
- `VU-rojo-indirect-0015` `maxCount` is greater than zero and at most
  `Limits::maxDrawIndirectCount` ([limits](15-formats-and-limits-part2.md#limits)).
- `VU-rojo-indirect-0016` A non-null `count` is 4-byte aligned and lies in a buffer created with
  `BufferUsage::IndirectArgs`.
- `VU-rojo-indirect-0017` Every GPU write on the same queue to the records or the count is ordered
  before this command by a barrier whose destination stage and access include `IndirectArgs`.
  Checked by the validation tracker.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::RenderCommandEncoder::drawMeshThreadgroups` with an indirect buffer address when `maxCount` is 1 and `count` is null; otherwise a kernel-encoded indirect command buffer | `ExecuteIndirect` with a root constant and `D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH`; the resource and offset are looked up from each `Addr`'s heap index | the device-address forms of `vkCmdDrawMeshTasksIndirectEXT` and `vkCmdDrawMeshTasksIndirectCountEXT` from `VK_KHR_device_address_commands` (entry-point names unverified) |
| Cost | costly: one extra dispatch and indirect-command-buffer memory; native when `maxCount` is 1 and `count` is null; spike 3 (pending) | cheap: index→resource lookup for `args` and `count` | native: entry points unverified |

### `CommandList::dispatchIndirect`

```cpp
void CommandList::dispatchIndirect(Addr args);
```

Dispatches the bound compute pipeline with the threadgroup counts of the `DispatchIndirectArgs`
record at `args`. The threadgroup size comes from the pipeline.

#### Valid usage
- `VU-rojo-indirect-0018` Recorded outside a render pass with a compute pipeline bound.
- `VU-rojo-indirect-0019` `args` is non-null and 4-byte aligned, and the record lies in a buffer
  created with `BufferUsage::IndirectArgs`.
- `VU-rojo-indirect-0020` Every GPU write on the same queue to the record is ordered before this
  command by a barrier whose destination stage and access include `IndirectArgs`. Checked by the
  validation tracker.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::ComputeCommandEncoder::dispatchThreadgroups` with an indirect buffer address, on the lazily opened compute encoder | `ExecuteIndirect` with a `D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH` command signature; the resource and offset are looked up from the `Addr`'s heap index | the device-address form of `vkCmdDispatchIndirect` from `VK_KHR_device_address_commands` (entry-point names unverified) |
| Cost | native | cheap: index→resource lookup for `args` | native: entry points unverified |
