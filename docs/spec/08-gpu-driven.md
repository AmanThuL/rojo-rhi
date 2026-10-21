# 08. GPU-driven rendering

**Status**: Accepted
**API version**: 0.1.0

GPU-driven work reads its arguments from GPU memory when it executes. A shader, usually a culling
pass, writes argument records and optionally a draw count; a barrier orders those writes before the
indirect command; the command then issues up to `maxCount` draws that all share the root block bound
by [`CommandList::setRoot`](05-shader-abi.md). Because one root serves the whole batch, every
indirect draw record begins with a `uint32` draw identifier, which the shader reads through
[`rojoRHI::drawId`](05-shader-abi-part3.md#rojorhidrawid) to find its own per-draw data.

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

Metal 4 has no multi-draw with a GPU count. Ahead of the render encoder that executes them, its
backend runs a compute kernel that encodes the records below `min(count, maxCount)` into an
indirect command buffer, with each `drawId` as its draw's base instance, and writes the execution
range the encoder runs that buffer with. A pass opens its render encoder lazily, at its first other
draw, a scope boundary or `endRenderPass`, so the kernels of a pass's leading counted draws share
one compute encoder ahead of it. A counted draw recorded after the render encoder opened splits
the pass: that encoder stores every attachment, resolving one that has a resolve target, the kernel
runs, and a new encoder resumes the pass with `Load`. That path is `costly`: per batch of kernels,
a compute encoder of five native calls and a backend barrier, and, on a device with scope timings,
the pass's begin timestamp when the batch opens the pass ([diagnostics](14-diagnostics.md)); per
counted draw, two native calls (its parameters and its dispatch) ahead of the execute, as the native
form makes; per split, a store and a load of every attachment; about 40 µs of fixed GPU time
measured alone, then about 48 ns per drawn record against 9.5 ns for a native indirect draw; and
indirect-command-buffer memory sized by `maxCount` rounded up to a power of two, at least 64
commands. With a `maxCount` of 1 the kernel writes one native record instead, empty when the count
is 0, for one plain indirect draw; only a lone mesh record with a null count skips the kernel.
Metal's shader validation silently drops commands such a kernel encodes, so conformance checks their
images under API validation only. [Spike 3](../research/2026-10-12-spike-3-multi-draw-indirect.md)
measured these lowerings against a `maxCount` loop of plain indirect draws, which fails the budget
by `maxCount`.

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

- `drawId`: caller-defined value that `rojoRHI::drawId` returns in every invocation of this draw.
- `vertexCount`: vertices per instance; zero makes the record a no-op.
- `instanceCount`: instances; zero makes the record a no-op.
- `firstVertex`: first vertex index; `rojoRHI::vertexIndex` includes it, provisionally on D3D12
  ([unverified](05-shader-abi-part3.md#rojorhivertexindex)).
- `firstInstance`: first instance index. It is not visible to shaders:
  [`rojoRHI::instanceIndex`](05-shader-abi-part3.md#rojorhiinstanceindex) is zero-based, and
  per-draw data comes through `drawId`.

`firstVertex + vertexCount` and `firstInstance + instanceCount` must not exceed 2^32 - 1.

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Draw fields | `MTL::DrawPrimitivesIndirectArguments` written by the backend kernel from record offset 4, with `drawId` in place of `firstInstance` | `D3D12_DRAW_ARGUMENTS` after a root-constant argument in the command signature | `VkDrawIndirectCommand` at record offset 4, stride 20 |
| `drawId` | written by the backend kernel as the draw's base instance, which the shader reads as `[[base_instance]]` | root constant read from record offset 0 | the backend binds the records' base address and stride, and the shader loads offset 0 of record `DrawIndex` |
| Cost | native: the rewrite is part of the command's cost | native | cheap: one load for `drawId` |

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

- `drawId`: caller-defined value that `rojoRHI::drawId` returns in every invocation of this draw.
- `indexCount`: indices per instance; zero makes the record a no-op.
- `instanceCount`: instances; zero makes the record a no-op.
- `firstIndex`: first element read from the index buffer.
- `baseVertex`: signed value added to each fetched index; `rojoRHI::vertexIndex` includes it,
  provisionally on D3D12 ([unverified](05-shader-abi-part3.md#rojorhivertexindex)).
- `firstInstance`: first instance index. It is not visible to shaders:
  [`rojoRHI::instanceIndex`](05-shader-abi-part3.md#rojorhiinstanceindex) is zero-based, and
  per-draw data comes through `drawId`.

`firstIndex + indexCount` elements of the draw's index type must fit in the index buffer, and
`firstInstance + instanceCount` must not exceed 2^32 - 1.

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Draw fields | `MTL::DrawIndexedPrimitivesIndirectArguments` written by the backend kernel from record offset 4, with `drawId` in place of `firstInstance` | `D3D12_DRAW_INDEXED_ARGUMENTS` after a root-constant argument | `VkDrawIndexedIndirectCommand` at record offset 4, stride 24 |
| `drawId` | written by the backend kernel as the draw's base instance, which the shader reads as `[[base_instance]]` | root constant read from record offset 0 | the backend binds the records' base address and stride, and the shader loads offset 0 of record `DrawIndex` |
| Cost | native: the rewrite is part of the command's cost | native | cheap: one load for `drawId` |

### `DrawMeshTasksIndirectArgs`

```cpp
struct DrawMeshTasksIndirectArgs { // 16 bytes, 4-byte aligned
    uint32_t drawId;
    uint32_t x, y, z;
};
```

One mesh-task draw read by `CommandList::drawMeshTasksIndirect`.

- `drawId`: caller-defined value that `rojoRHI::drawId` returns in every invocation of this draw.
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
| `drawId` | the backend binds the record's address for each draw, inside the indirect command buffer when a kernel encodes the batch, and the shader loads offset 0 | root constant read from record offset 0 | the backend binds the records' base address and stride, and the shader loads offset 0 of record `DrawIndex` |
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
- `VU-rojo-indirect-0021` A viewport and a scissor have been set since `beginRenderPass`.
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
| Call | a backend kernel ahead of the render encoder encodes the records below `min(count, maxCount)` into an indirect command buffer, which `MTL4::RenderCommandEncoder::executeCommandsInBuffer` runs with the range the kernel wrote; recorded after the render encoder opened, it splits the pass, which resumes with `Load`; when `maxCount` is 1, the kernel writes one native record instead, with no instances when `count` is 0, for one `MTL4::RenderCommandEncoder::drawPrimitives` with an indirect buffer address | `ExecuteIndirect` with a root constant and `D3D12_INDIRECT_ARGUMENT_TYPE_DRAW` in the command signature; `count` as the count buffer; the resource and offset are looked up from each `Addr`'s heap index | the device-address forms of `vkCmdDrawIndirect` and `vkCmdDrawIndirectCount` from `VK_KHR_device_address_commands`, which the oracle assumes ([ADR 0010](../decisions/0010-hardware-floor.md)) (entry-point names unverified) |
| Cost | costly: two native calls per draw ahead of the execute, a compute encoder and a barrier per batch, a store and a load of every attachment per split, about 48 ns of GPU time per drawn record against 9.5 ns native, and about 670 bytes of indirect-command-buffer memory per command ([spike 3](../research/2026-10-12-spike-3-multi-draw-indirect.md)) | cheap: index→resource lookup for `args` and `count` | native: entry points unverified |

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
- `VU-rojo-indirect-0022` A viewport and a scissor have been set since `beginRenderPass`.
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
| Call | a kernel-encoded indirect command buffer ahead of the render encoder, splitting the pass when recorded after it opened, as for `drawIndirect`; when `maxCount` is 1, the kernel writes one native record for one `MTL4::RenderCommandEncoder::drawIndexedPrimitives` with an indirect buffer address | `IASetIndexBuffer` with a view of the whole buffer by GPU virtual address, unless the list holds that view, then `ExecuteIndirect` with a root constant and `D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED`; the resource and offset are looked up from each `Addr`'s heap index | `vkCmdBindIndexBuffer2`, then the device-address forms of `vkCmdDrawIndexedIndirect` and `vkCmdDrawIndexedIndirectCount` from `VK_KHR_device_address_commands` (entry-point names unverified) |
| Cost | costly: as for `drawIndirect`, with about 690 bytes of indirect-command-buffer memory per record ([spike 3](../research/2026-10-12-spike-3-multi-draw-indirect.md)) | cheap: index→resource lookup for `args` and `count` | native: entry points unverified |

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
- `VU-rojo-indirect-0023` A viewport and a scissor have been set since `beginRenderPass`.
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
| Call | when `maxCount` is 1 and `count` is null, `MTL4::RenderCommandEncoder::drawMeshThreadgroups` with the indirect buffer address at record offset 4 and the record bound at `buffer(3)`, on the render encoder with no kernel; when `maxCount` is 1 with a count, the kernel writes one native record, with no groups when `count` is 0, for one such draw; above 1, a kernel-encoded indirect command buffer as for `drawIndirect`, whose commands inherit no buffer and bind the root recorded with the call, the view and sampler tables and their own record at `buffer(0)` to `buffer(3)` on the object, mesh and fragment stages | `ExecuteIndirect` with a root constant and `D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH`; the resource and offset are looked up from each `Addr`'s heap index | the device-address forms of `vkCmdDrawMeshTasksIndirectEXT` and `vkCmdDrawMeshTasksIndirectCountEXT` from `VK_KHR_device_address_commands` (entry-point names unverified) |
| Cost | costly: as for `drawIndirect`, with 1,536 bytes of indirect-command-buffer memory per command for its twelve bindings as measured on an Apple M3 Max, against 1,280 with one in [spike 3](../research/2026-10-12-spike-3-multi-draw-indirect.md), checked for correctness and not timed; native when `maxCount` is 1 and `count` is null | cheap: index→resource lookup for `args` and `count` | native: entry points unverified |

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
