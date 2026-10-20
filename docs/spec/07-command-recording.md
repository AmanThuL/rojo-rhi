# 07. Command recording

**Status**: Accepted
**API version**: 0.1.0

A command list comes from a per-frame pool and returns to it when its frame retires. One thread
records one list at a time; many lists record in parallel and are submitted in order ([queues and
frames](10-queues-and-frames.md)). In Release builds recording takes no locks, except when a list
claims a new frame-arena page, for `uploadFrameData` or for the label of a debug scope (including
those of render passes and upscales); a Debug-tier check may take the lock of the pool whose record
it reads ([ADR 0020](../decisions/0020-recording-locks-scope-labels.md)). Metal 4 columns describe
the implemented backend; D3D12 columns describe the planned backend; Vulkan columns are the design
oracle. This chapter continues in [draws, dispatches and scopes](07-command-recording-part2.md) and
[copies and clears](07-command-recording-part3.md).

**Only render passes have scope.** Compute and copy commands are valid anywhere outside a render
pass; there is no `beginComputePass` or `beginCopyPass`. On D3D12 and Vulkan such scopes would be
empty, as neither has encoders. On Metal 4 the backend opens a compute encoder lazily on the first
compute or copy command and closes it at the next `beginRenderPass`, scope boundary or `end`, an
ordering barrier or a render-target `clearTexture`. Copies record on the compute encoder, so merged
work avoids encoder switches and uses the lighter intra-encoder barrier, not the queue-level one.
Work inside one encoder is ordered only by barriers ([synchronization](09-synchronization.md)): two
dispatches, or a dispatch and a copy, without a barrier between them may overlap.

**Render passes perform no layout transitions.** Attachments arrive in their attachment layouts
through explicit barriers recorded before `beginRenderPass`. Barriers inside a render pass are
invalid in 0.1.0, since D3D12 render passes and Vulkan dynamic rendering both restrict them.

The rules in the entries combine into this summary:

| Command | Outside a render pass | Inside a render pass | Queue types |
|---|---|---|---|
| `setPipeline` with a compute pipeline | yes | no | Graphics, Compute |
| `setPipeline` with a graphics pipeline; dynamic state; draws | no | yes | Graphics |
| `dispatch`, `dispatchIndirect` | yes | no | Graphics, Compute |
| `copyBuffer`, `copyBufferToTexture`, `copyTextureToBuffer`, `copyTexture` | yes | no | all |
| `fillBuffer`, `clearTexture` | yes | no | Graphics, Compute |
| `barrier` | yes | no | all |
| `beginScope`, `endScope` | yes | yes | all |

### `QueueType`

```cpp
enum class QueueType : uint8_t { Graphics, Compute, Copy };
```

The queue a command list is recorded for and submitted to. `Graphics` accepts every command,
`Compute` accepts compute, copy, fill, clear and scope commands, and `Copy` accepts copy and scope
commands, as the summary table shows.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | one `MTL4::CommandQueue` per type; Metal 4 queues are not typed | `D3D12_COMMAND_LIST_TYPE_DIRECT`, `COMPUTE`, `COPY` | queue families with graphics, compute or transfer support |
| Cost | native | native | native |

### `Device::acquireCommandList`

```cpp
CommandList Device::acquireCommandList(QueueType type, std::string_view label);  // per-frame pool
```

Returns a command list in the recording state from the open frame's pool. The list belongs to that
frame: it returns to the pool when the frame retires, and its handle is valid until then. A use of
the handle afterwards is undefined behaviour that no tier detects, since the pool may hand the same
list out again. The call is free-threaded. The label defaults to the `rojorhi.` prefix
([fundamentals](01-fundamentals.md)). Every list acquired in a frame is ended and submitted before
that frame's `Device::endFrame`, as `VU-rojo-device-0005` of
[`Device::endFrame`](02-device.md#deviceendframe) requires.

#### Valid usage

- `VU-rojo-cmd-0001` Called between `Device::beginFrame` and `Device::endFrame`
  ([device](02-device.md)).

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | pooled `MTL4::CommandBuffer`, `MTL4::CommandBuffer::beginCommandBuffer` with the list's own `MTL4::CommandAllocator` of its frame slot, reset when the slot is reused | `Reset` with a per-frame allocator of the list type; for `Graphics` and `Compute` lists, then the descriptor heaps and the global root signature | `vkBeginCommandBuffer` from a per-frame pool; for `Graphics` and `Compute` lists, then the descriptor heaps |
| Cost | native | native | native |

### `CommandList::end`

```cpp
void CommandList::end();
```

Ends recording; the list records nothing further and is ready for `Queue::submit`. On Metal 4 it
carries a barrier still pending on the open encoder, or else on an encoder of its own, and ends it.

#### Valid usage

- `VU-rojo-cmd-0002` The list is recording: `end` has not been called on it.
- `VU-rojo-cmd-0003` No render pass is open; a render pass begins and ends in one list.
- `VU-rojo-cmd-0004` Every scope begun on the list has ended.
- `VU-rojo-cmd-0063` Every command other than `end` is recorded on a list that is recording: `end`
  has not been called on it.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | for a barrier still pending, `MTL4::CommandEncoder::updateFence` of the graphics queue's carry fence on a `Graphics` list that uses it (chapter 09) and `barrierAfterStages` on the open encoder when it waited for every source stage, else on a compute encoder that holds no command after `barrierAfterQueueStages` ([synchronization](09-synchronization-part2.md#commandlistbarrier)); `endEncoding` on each; then `MTL4::CommandBuffer::endCommandBuffer` | `Close` | `vkEndCommandBuffer` |
| Cost | native | native | native |

### `LoadOp`

```cpp
enum class LoadOp : uint8_t { Load, Clear, DontCare };
```

What an attachment holds when a render pass begins: its earlier contents, the attachment's clear
value, or undefined contents the pass must overwrite.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::LoadActionLoad`, `MTL::LoadActionClear`, `MTL::LoadActionDontCare` | `D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE`, `CLEAR`, `DISCARD` | `VK_ATTACHMENT_LOAD_OP_LOAD`, `CLEAR`, `DONT_CARE` |
| Cost | native | native | native |

### `StoreOp`

```cpp
enum class StoreOp : uint8_t { Store, DontCare };
```

Whether an attachment's rendered contents are kept when the render pass ends. With `DontCare` the
contents are undefined afterwards; a multisample resolve still happens.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::StoreActionStore`, `MTL::StoreActionDontCare`, or the resolve forms | `D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE`, `DISCARD`, or `RESOLVE` | `VK_ATTACHMENT_STORE_OP_STORE`, `DONT_CARE` |
| Cost | native | native | native |

### `ColorAttachment`

```cpp
struct ClearColor { float r = 0.0f, g = 0.0f, b = 0.0f, a = 0.0f; };
struct ColorAttachment {
    Texture texture; uint32_t mip = 0, layer = 0;
    LoadOp load = LoadOp::Load; StoreOp store = StoreOp::Store; ClearColor clear;
    Texture resolve = {};
};
```

One color attachment: a single mip and array layer (a cube face counts as a layer) of `texture`.
`clear` is used when `load` is `Clear`; integer formats receive the values converted to integers.
A non-null `resolve` receives the average of the samples at the end of the pass, written to its
mip 0 and layer 0. 3D textures are not attachments in 0.1.0.

#### Valid usage

- `VU-rojo-cmd-0005` `texture` was created with `TextureUsage::RenderTarget`, is not a 3D texture,
  and `mip` and `layer` lie within it.
- `VU-rojo-cmd-0006` Retired in 0.1.0: A non-null `resolve` requires `texture` to have more than
  one sample, and is a single-sample `RenderTarget` texture of the same format and of the extent of
  `texture` at `mip`, whose format capabilities report `resolve`. The capability is
  `VU-rojo-format-0003` ([formats and limits](15-formats-and-limits.md)); the rest is
  `VU-rojo-cmd-0071`.
- `VU-rojo-cmd-0071` A non-null `resolve` requires `texture` to have more than one sample, and is
  a single-sample `RenderTarget` texture of the same format and of the extent of `texture` at `mip`.
- `VU-rojo-cmd-0007` At `beginRenderPass`, the subresource and the resolve target are in
  `Layout::ColorAttachment` ([synchronization](09-synchronization.md)). Checked by the validation
  tracker.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::RenderPassColorAttachmentDescriptor` (`setLevel`, `setSlice`, `setResolveTexture`) | `D3D12_RENDER_PASS_RENDER_TARGET_DESC` with a CPU render-target view | `VkRenderingAttachmentInfo`, resolve mode average |
| Resolve layout | layouts ignored | resolve target moved to `RESOLVE_DEST` and back by backend barriers | `COLOR_ATTACHMENT_OPTIMAL` |
| Cost | native | cheap: backend-cached view per subresource and the resolve barriers | cheap: backend-cached image view per subresource |

### `DepthAttachment`

```cpp
struct DepthAttachment {
    Texture texture; uint32_t mip = 0, layer = 0;
    LoadOp load = LoadOp::Load; StoreOp store = StoreOp::Store; float clearDepth = 0.0f;
    LoadOp stencilLoad = LoadOp::DontCare; StoreOp stencilStore = StoreOp::DontCare;
    uint8_t clearStencil = 0;
};
```

The depth and stencil attachment of a render pass; a null `texture` means none. `load`, `store`
and `clearDepth` govern depth; the stencil fields govern stencil and are ignored for formats
without one. Depth resolve and read-only depth attachments are not part of 0.1.0.

#### Valid usage

- `VU-rojo-cmd-0008` A non-null `texture` was created with `TextureUsage::DepthStencil` in a depth
  or depth-stencil format, and `mip` and `layer` lie within it.
- `VU-rojo-cmd-0009` `clearDepth` lies in `[0, 1]`.
- `VU-rojo-cmd-0010` At `beginRenderPass`, the subresource is in `Layout::DepthStencilWrite`.
  Checked by the validation tracker.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::RenderPassDepthAttachmentDescriptor` and `MTL::RenderPassStencilAttachmentDescriptor` | `D3D12_RENDER_PASS_DEPTH_STENCIL_DESC` with a CPU depth-stencil view | `VkRenderingAttachmentInfo` for depth and for stencil |
| Cost | native | cheap: backend-cached view per subresource | cheap: backend-cached image view per subresource |

### `RenderPassDesc`

```cpp
struct RenderPassDesc {
    std::span<const ColorAttachment> colors; DepthAttachment depth; Rect2D area;
    std::string_view label;
};
```

The attachments of one render pass, the area it renders to, and the label of the scope the pass
forms. `Rect2D` is defined in [fundamentals](01-fundamentals.md#rect2d).
Load and store operations may affect the whole attachment subresource, not only `area`, because
Metal 4 and D3D12 render passes carry no render area.

#### Valid usage

- `VU-rojo-cmd-0011` `colors` holds at most `Limits::maxColorAttachments` entries, and the pass
  has at least one attachment.
- `VU-rojo-cmd-0012` Every attachment has the same extent at its mip, and every attachment other
  than a resolve target has the same sample count.
- `VU-rojo-cmd-0013` `area` has non-zero width and height and lies within the attachments' extent.
- `VU-rojo-cmd-0014` No subresource appears twice among the attachments and resolve targets.
- `VU-rojo-cmd-0015` The color attachments' formats together take at most
  `Limits::maxColorBytesPerSample` bytes per sample, counted as
  [limits](15-formats-and-limits-part2.md#limits) defines: the sum of each format's bytes per texel.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::RenderPassDescriptor`; no render area | `BeginRenderPass` arguments; no render area | `VkRenderingInfo` with `renderArea` |
| Cost | native | native | native |

### `CommandList::beginRenderPass`

```cpp
void CommandList::beginRenderPass(const RenderPassDesc& desc);
```

Begins a render pass. It performs the load operations and no layout transition. The pass forms its
own scope, a debug group and a timestamp pair labeled `desc.label` (see `CommandList::beginScope`).
Dynamic state starts as listed in [dynamic state](06-pipelines-part2.md), and no pipeline is bound.
On Metal 4 it closes the lazily opened compute encoder and opens no encoder: the render encoder
opens at the pass's first draw that needs no kernel, at a scope boundary inside the pass, or at
`endRenderPass` ([GPU-driven](08-gpu-driven.md) gives counted draws).

#### Valid usage

- `VU-rojo-cmd-0016` Recorded on a `Graphics` command list with no render pass open.
- `VU-rojo-cmd-0017` Retired in 0.1.0: Until `endRenderPass`, the list records no barrier, dispatch,
  copy, fill or clear. Each of those commands states it: `VU-rojo-sync-0019`, `VU-rojo-cmd-0029`,
  `VU-rojo-indirect-0018`, `VU-rojo-cmd-0042`, `0046`, `0049`, `0052`, `0056` and `0060`.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::CommandBuffer::pushDebugGroup` for the pass's scope, whose timestamp pair its encoders write ([diagnostics](14-diagnostics.md)): the begin timestamp first on the pass's first encoder, the compute encoder of its leading counted draws' kernels or else its render encoder; `MTL4::CommandBuffer::renderCommandEncoder` deferred to the pass's first draw that needs no kernel, scope boundary inside it, or `endRenderPass`, with the caller's load actions, and `Load` when a split resumes the pass | `BeginRenderPass` (`ID3D12GraphicsCommandList4`) | `vkCmdBeginRendering` |
| Cost | native | cheap: starting dynamic state written | native |

### `CommandList::endRenderPass`

```cpp
void CommandList::endRenderPass();
```

Ends the open render pass: performs store operations and resolves, closes the pass's scope and
unbinds the pipeline. The next compute or copy command opens a new Metal 4 compute encoder.

#### Valid usage

- `VU-rojo-cmd-0018` A render pass is open on this list.
- `VU-rojo-cmd-0019` Every scope begun inside the render pass has ended.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::CommandBuffer::renderCommandEncoder` first when nothing opened it, which then writes both timestamps; `MTL4::RenderCommandEncoder::setColorStoreAction`, `setDepthStoreAction` or `setStencilStoreAction` for an attachment that is not `Store` without a resolve; `MTL4::RenderCommandEncoder::writeTimestamp` for the pass's end timestamp; `MTL4::CommandBuffer::popDebugGroup`; the encoder stays open, running no further command, and `endEncoding` follows at the next encoder, scope boundary or `end`, which may carry a barrier the list still owes on it ([synchronization](09-synchronization-part2.md#commandlistbarrier)) | `EndRenderPass` | `vkCmdEndRendering` |
| Cost | native | native | native |
