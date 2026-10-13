# 09. Synchronization, part 2

**Status**: Accepted
**API version**: 0.1.0

This part defines the three barrier kinds, the one batched call that records them, the cross-queue
handoff of textures and the first use of aliased memory. The stage, access and layout vocabulary
is in [part 1](09-synchronization.md).

Each barrier kind exists for a reason. A global barrier orders memory by stage and access with no
resource, and any buffer hazard can be expressed with one. A buffer barrier names a buffer and a
range, so the validation tracker clears exactly the named range and D3D12 and Vulkan receive a
resource-scoped barrier where the caller wants one. A texture barrier exists because D3D12 needs
layouts: render-target and depth compression apply only in their own layouts, so a general-layout
policy costs GPU time there, while Vulkan's unified image layouts make the general layout cheap
only on Vulkan.

On Metal 4 the whole batch collapses into one stage barrier, so naming resources costs Metal
nothing. That answers the regression of a stage-only design, which multiplies barrier counts on
isolated hazards because it cannot name a resource.

## Barriers

### `GlobalBarrier`

```cpp
struct GlobalBarrier {
    Stage srcStage; Access srcAccess;
    Stage dstStage; Access dstAccess;
};
```

A memory dependency over all memory, with no resource named and no layout changed.

- `srcStage`, `srcAccess`: stages and accesses of earlier work that the barrier waits for and makes
  visible.
- `dstStage`, `dstAccess`: stages and accesses of later work that wait and see the source writes.

The first use of a placed buffer whose memory another resource used earlier is ordered by a global
barrier: its source covers the last use of the earlier resource and its destination covers the
first use of the buffer.

#### Valid usage
- `VU-rojo-sync-0008` `srcStage` and `dstStage` are not both `Stage::None`.
- `VU-rojo-sync-0009` The first use of a placed buffer whose memory overlaps a resource used earlier
  on the queue follows a global barrier whose source covers that resource's last use and whose
  destination covers the buffer's first use. Checked by the validation tracker.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Lowering | folded into the batch's stage barrier | `D3D12_GLOBAL_BARRIER` | `VkMemoryBarrier2` |
| Cost | native | native | native |

### `BufferBarrier`

```cpp
inline constexpr uint64_t kWholeSize = ~uint64_t{0};

struct BufferBarrier {
    Buffer buffer; uint64_t offset = 0, size = kWholeSize;
    Stage srcStage; Access srcAccess;
    Stage dstStage; Access dstAccess;
};
```

A memory dependency scoped to one range of one buffer.

- `buffer`: the buffer whose memory the barrier orders.
- `offset`, `size`: the byte range; `kWholeSize` extends it to the end of the buffer.
- `srcStage`, `srcAccess`, `dstStage`, `dstAccess`: as for `GlobalBarrier`.

The validation tracker treats exactly the named range as ordered. D3D12 widens the range to the
whole buffer, because enhanced buffer barriers require offset 0 and the full size; the result
orders more than asked, never less.

#### Valid usage
- `VU-rojo-sync-0010` `offset` is less than the buffer's size, and `size` is `kWholeSize` or is
  greater than zero with `offset + size` at most the buffer's size.
- `VU-rojo-sync-0011` `srcAccess` and `dstAccess` name only `IndirectArgs`, `IndexRead`,
  `ShaderRead`, `ShaderWrite`, `CopyRead` and `CopyWrite`.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Lowering | folded into the batch's stage barrier | `D3D12_BUFFER_BARRIER` with `Offset` 0 and `Size` `UINT64_MAX`: the range is widened to the whole buffer | `VkBufferMemoryBarrier2`, range honoured |
| Cost | native | native | native |

### `TextureBarrier`

```cpp
struct TextureBarrier {
    Texture texture; SubresourceRange range;
    Stage srcStage; Access srcAccess;
    Stage dstStage; Access dstAccess;
    Layout oldLayout, newLayout;          // oldLayout = Undefined discards contents
};
```

A memory dependency and layout transition for a range of one texture.

- `texture`: the texture whose subresources the barrier orders.
- `range`: the mips and layers covered ([`SubresourceRange`](04-textures-and-heaps.md)).
- `srcStage`, `srcAccess`, `dstStage`, `dstAccess`: as for `GlobalBarrier`.
- `oldLayout`: the layout every covered subresource is in; `Undefined` discards the contents.
- `newLayout`: the layout every covered subresource is in after the barrier.

#### Valid usage
- `VU-rojo-sync-0012` Retired in 0.1.0: `range` lies within the texture's mips and layers.
  `VU-rojo-texture-0016`, `0017` and `0041` state the rules of every subresource range once, for
  views, clears and barriers alike ([textures and heaps](04-textures-and-heaps.md)).
- `VU-rojo-sync-0013` `oldLayout` is `Undefined` or the layout every subresource in `range` is in at
  this point in queue submission order. Checked by the validation tracker.
- `VU-rojo-sync-0014` `newLayout` is not `Undefined`.
- `VU-rojo-sync-0015` `newLayout` is allowed by the texture's usage: `ShaderRead` needs `Sampled`,
  `ShaderWrite` needs `Storage`, `ColorAttachment` needs `RenderTarget`, and `DepthStencilWrite`
  and `DepthStencilRead` need `DepthStencil`.
- `VU-rojo-sync-0016` `srcAccess` is permitted by `oldLayout`, or is `None` when `oldLayout` is
  `Undefined`, and `dstAccess` is permitted by `newLayout`.
- `VU-rojo-sync-0017` The first use of a placed texture whose memory overlaps a resource used
  earlier on the queue is a texture barrier from `Undefined` whose source covers that resource's
  last use. Checked by the validation tracker.
- `VU-rojo-sync-0018` A texture used on one queue and next used on another is, at the handoff, in a
  layout both queues accept: `General` when either queue is the `Copy` queue, and `General`,
  `ShaderRead`, `ShaderWrite`, `CopySrc` or `CopyDst` between the `Graphics` and `Compute` queues.
  The acquiring queue's first barrier starts from that layout or discards from `Undefined`. Checked
  by the validation tracker.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Lowering | folded into the batch's stage barrier; layouts ignored | `D3D12_TEXTURE_BARRIER` with `LayoutBefore` and `LayoutAfter`; `Undefined` adds `D3D12_TEXTURE_BARRIER_FLAG_DISCARD` | `VkImageMemoryBarrier2` with `oldLayout` and `newLayout` |
| Cost | native | native | native |

A barrier from `Undefined` names no source access (`VU-rojo-sync-0016`), yet it still orders the
writes before it (`VU-rojo-sync-0024`). On Vulkan it lowers with `srcAccessMask` widened to the
write accesses its source stages perform, so those writes are made available before the layout
changes. On D3D12 it lowers with `AccessBefore` `D3D12_BARRIER_ACCESS_NO_ACCESS` paired with its
`SyncBefore` stages, taken to wait for those writes (*unverified*).

### `CommandList::barrier`

```cpp
void CommandList::barrier(std::span<const GlobalBarrier> globals,
                          std::span<const BufferBarrier> buffers = {},
                          std::span<const TextureBarrier> textures = {});
```

Records one batch of barriers. Every barrier in the batch takes effect at the same point: all
source scopes complete, then all layout transitions, then all destination scopes may begin. An
empty batch records nothing.

#### Valid usage
- `VU-rojo-sync-0019` Recorded outside a render pass.
- `VU-rojo-sync-0020` No two entries of `textures` cover the same subresource.
- `VU-rojo-sync-0021` Between two uses of the same memory on one queue in which at least one writes,
  other than the uses one render pass orders itself, a barrier lies in queue submission order.
  Checked by the validation tracker.
- `VU-rojo-sync-0022` Retired in 0.1.0: Between two uses of a texture subresource in different
  layouts, a texture barrier moves it from the first layout to the second.
- `VU-rojo-sync-0023` Retired in 0.1.0: Such a barrier covers both uses: its source stage and
  access include the earlier use's, and its destination stage and access include the later use's.
- `VU-rojo-sync-0024` Between two uses of the same memory on one queue in which at least one writes,
  one barrier's source covers the earlier use and its destination covers the later use. A source
  covers a use when its stage includes the use's stages and its access includes the use's write
  accesses; a barrier from `Undefined` needs no source access, and a source with no stage covers no
  use. A destination covers a use when its stage includes the use's stages and, when the earlier use
  writes, its access includes the use's accesses. For a write after a write, a read between them
  also orders the pair when a barrier before the read covers the first write and the read, and one
  after it covers the read and the second write. A texture barrier that changes the layout is a
  write between the uses around it, ordered after the uses before it as a write is, with its own
  source as one more barrier; with no use since the last layout change, its source stage shares a
  stage with that change's destination stage or with the destination of a later barrier whose source
  stage does. The next use is covered by the change's destination, or by the destination of a later
  barrier whose source stage shares a stage with the change's destination stage. Two barriers of one
  `CommandList::barrier` call never order a pair together. Checked by the validation tracker.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Batch | one stage barrier from the union of source stages to the union of destination stages: `MTL4::ComputeCommandEncoder::barrierAfterEncoderStages` (or the render encoder's) when all source work since the last queue-level barrier lies in the open encoder; otherwise a consumer `barrierAfterQueueStages` on the next encoder as well, which covers sources in earlier encoders and earlier command lists (the NRI lowering) | one `ID3D12GraphicsCommandList7::Barrier` call with one `D3D12_BARRIER_GROUP` per non-empty kind | one `vkCmdPipelineBarrier2` with one `VkDependencyInfo` |
| Visibility | `MTL4::VisibilityOptionDevice` when any access is named; `MTL4::VisibilityOptionResourceAlias` added when the batch holds a global barrier or a texture barrier from `Undefined` on a placed texture; both are tuning points for measurement | per barrier | per barrier |
| Cost | native: one barrier per batch, encoder-level or queue-level | native | native |

The Metal 4 visibility rules are deliberately conservative. Selecting `Device` visibility for any
named access, where NRI selects it only when a source access writes, and adding `ResourceAlias` to
every batch with a global barrier, are tuning points measured under
[ADR 0014](../decisions/0014-overhead-measurement.md) before they are narrowed.

## Cross-queue handoff

A texture handed between queues is, at the handoff, in a layout both queues accept
(`VU-rojo-sync-0018`). Between the graphics and compute queues that is `General`, `ShaderRead`,
`ShaderWrite`, `CopySrc` or `CopyDst`: on D3D12 they lower to `LAYOUT_COMMON` and the generic
`LAYOUT_SHADER_RESOURCE`, `LAYOUT_UNORDERED_ACCESS`, `LAYOUT_COPY_SOURCE` and `LAYOUT_COPY_DEST`,
which direct and compute queues both accept, so a texture created with initial data in
`ShaderRead` is read on either queue without a transition. A handoff to or from the copy queue is
in `General`, which maps to `D3D12_BARRIER_LAYOUT_COMMON`, taken to be the one layout every D3D12
queue type accepts (*unverified*; see [part 1](09-synchronization.md)). On Vulkan, resources are
shared concurrently between queue families, so no ownership transfer is needed and every layout is
valid on every queue that supports its use. Buffers need no transition. In every case the acquiring
queue waits on a fence the releasing queue signals after its last use
([queues and frames](10-queues-and-frames.md)). The validation tracker follows layouts per queue
and checks the handoff.

## First use of aliased memory

Placement ([memory and buffers](03-memory-and-buffers.md)) lets resources share memory; the policy
belongs to the caller's graph. The first use of a placed texture is a barrier from `Undefined`
(`VU-rojo-sync-0017`), which discards whatever the earlier resource left and, on D3D12,
initializes compression metadata. The first use of a placed buffer is a global barrier
(`VU-rojo-sync-0009`). Either barrier's source covers the last use of the resource that held the
memory before.
