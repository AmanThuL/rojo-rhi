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
- `VU-rojo-sync-0027` When `newLayout` is `DepthStencilRead`, the texture's usage also has
  `Sampled`: the layout permits only shader reads through `Sampled` views, and D3D12 lowers it to
  `LAYOUT_SHADER_RESOURCE`, which a texture created without `Sampled` cannot take.
- `VU-rojo-sync-0016` `srcAccess` is permitted by `oldLayout`, or is `None` when `oldLayout` is
  `Undefined`, and `dstAccess` is permitted by `newLayout`.
- `VU-rojo-sync-0017` The first use of a placed texture whose memory overlaps a resource used
  earlier on the queue is a texture barrier from `Undefined` whose source covers that resource's
  last use. Checked by the validation tracker.
- `VU-rojo-sync-0018` Retired in 0.1.0: A texture used on one queue and next used on another is,
  at the handoff, in a layout both queues accept: `General` when either queue is the `Copy` queue,
  and `General`, `ShaderRead`, `ShaderWrite`, `CopySrc` or `CopyDst` between the `Graphics` and
  `Compute` queues. The acquiring queue's first barrier starts from that layout or discards from
  `Undefined`. Checked by the validation tracker.
- `VU-rojo-sync-0025` A texture used on one queue and next used on another is, at the handoff, in a
  layout both queues accept, unless the acquiring queue's first barrier on it discards from
  `Undefined`: `General` when either queue is the `Copy` queue, and `General`, `ShaderRead`,
  `ShaderWrite`, `CopySrc` or `CopyDst` between the `Graphics` and `Compute` queues. A first
  barrier that does not discard starts from that layout. Checked by the validation tracker.

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
  a chain of barriers orders the earlier use before the later one. A barrier whose source covers
  the earlier use starts a chain; a later barrier whose source stage shares a stage with the
  destination stage of a barrier of the chain extends it; the destination of a barrier of the chain
  covers the later use. A source covers a use when its stage includes the use's stages and its
  access includes the use's write accesses; a barrier from `Undefined` needs no source access, and a
  source with no stage covers no use. A destination covers a use when its stage includes the use's
  stages and, when the earlier use writes, its access includes the use's accesses. For a write after
  a write, a read between them also orders the pair when a chain orders the first write before the
  read and another orders the read before the second write. A texture barrier that changes the
  layout is a write between the uses around it, ordered after the uses before it as a write is, with
  its own source as the last barrier of a chain, and it starts a chain as a barrier whose source
  covers it and whose destination is its own. Two barriers of one `CommandList::barrier` call never
  chain. Checked by the validation tracker.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Batch | one stage barrier from the union of source stages to the union of destination stages, each intersected with the stages the queue's encoders run: compute and render stages on `Graphics`, `MTL::StageDispatch \| MTL::StageBlit` on `Compute`, and `MTL::StageBlit` on `Copy`, whose commands are all blits; a batch left with no source or no destination stage emits nothing. While the open encoder holds work, the sources lie within those its opening barrier waited for, and the destination stages it has within those that barrier held back, the batch stays in the encoder: `MTL4::ComputeCommandEncoder::barrierAfterEncoderStages` from the sources it has to a destination stage it has, when both stage sets are non-empty, emitted just before the encoder's next command in that stage, since it orders only later commands of its own encoder; its other destination stages stay held for a later command in them, consecutive batches merge, and if the encoder ends first what is still held becomes pending instead. The destination stages the encoder lacks become pending. Otherwise, when the open encoder is a compute encoder whose opening barrier waited for nothing, which runs more than a counted draw's kernels and has run no command in the destination stages it has, the batch stays in it too: `barrierAfterQueueStages` from the source stages to those destination stages, at once, for the work of earlier encoders, and the encoder-level barrier for its own work held back as above; since every command of the encoder in those stages follows it, the queue-level barrier then stands as its opening barrier. Otherwise the open encoder ends and the batch becomes pending. A compute encoder that opens while a barrier is pending first emits `barrierAfterQueueStages` from the pending source stages to the pending destination stages it has, its opening barrier, and those stop pending; a render encoder that a `clearTexture` opens emits it only for `MTL::StageFragment`, where its load and store actions run, and the other render stages stay pending; this orders sources in earlier encoders and earlier command lists (the NRI lowering). When an encoder ends, the stages its opening barrier held back in which it ran no command become pending again, since the barrier ordered only its own work. A barrier still pending at [`CommandList::end`](07-command-recording.md) is carried to later lists. A render pass's last encoder stays open after `endRenderPass`, running no further command, until the next encoder, a scope boundary or `end`; when every pending source stage lies among those its opening barrier waited for and it ran a command in a stage that barrier held, it carries the barrier itself, after the stages it ran: on a `Graphics` list that uses the queue's carry `MTL::Fence`, `updateFence` of that fence for the destination's `MTL::StageDispatch`, and `barrierAfterStages` to the destination stages left. Otherwise a compute encoder that holds no command emits `barrierAfterQueueStages` from the pending source stages to its stages, then the same `updateFence` and `barrierAfterStages` from its stages. Every other compute encoder of such a list calls `waitForFence` on the carry fence before its first command in `MTL::StageDispatch`, while the compute encoder of a counted draw's kernels ([GPU-driven](08-gpu-driven.md)) does not wait, since the kernels read only records that `IndirectArgs` orders. A `Graphics` list uses the carry fence when it is acquired after a counted draw's kernels have run on the device in an earlier frame; before that no encoder would be spared a barrier to the dispatch stage, so a list keeps that stage in its `barrierAfterStages` and waits on no fence, and every such list is submitted before the frame of the first list that updates the fence begins. The fence update carries no visibility option: Metal orders it after the stages it names complete. A list whose barriers name `IndirectArgs` or `All` as a destination, or whose pending barrier's visibility includes `MTL4::VisibilityOptionResourceAlias`, puts the dispatch stage in the `barrierAfterStages` instead, which holds those kernels back too. A list that upscaled keeps a trailing encoder that waits on the upscale's fence and holds back every stage ([upscaling](12-upscaling.md)) | one `ID3D12GraphicsCommandList7::Barrier` call with one `D3D12_BARRIER_GROUP` per non-empty kind | one `vkCmdPipelineBarrier2` with one `VkDependencyInfo` |
| Visibility | `MTL4::VisibilityOptionDevice` when any access is named; `MTL4::VisibilityOptionResourceAlias` added only while memory aliases: to a batch with a global barrier while a live placed resource has overlapped another placed resource's bytes, living or destroyed, or a Sparse resource is live, and to a texture barrier from `Undefined` on a placed texture that has, or on any placed texture while a Sparse resource is live, and on any Sparse texture; `Device` selection is a tuning point for measurement. The dispatch stage of a barrier carried past `CommandList::end` through the carry fence takes no visibility option, so alias visibility keeps it in `barrierAfterStages` | per barrier | per barrier |
| Cost | native: a batch inside the open encoder is at most one encoder-level barrier per destination stage the encoder's later commands run in, plus one queue-level barrier when its sources lie outside the encoder, which saves the encoder that ending it would open (C6's fill, barrier and dispatches: 2.7 µs less CPU per list on an M3 Max, a diagnostic run of eight paired processes). Recording a batch of one global barrier that repeats the list's last such batch and stays in the open encoder reuses that batch's lowering and calls nothing, the entry being inlined into its caller, and the next command emits the held barrier inline (C6's barrier between dispatches: about 4.5 ns less CPU per repetition at `K` = 10,000 on an M3 Max, a diagnostic run of ten paired processes, unjudged); a pending batch emits nothing itself, and each encoder that opens while a barrier is pending emits one queue-level barrier. A barrier still pending at `end` costs the list's last encoder an `updateFence`, a `barrierAfterStages` or both, or else an encoder of its own with one or two barriers; each compute encoder that dispatches on a `Graphics` list that uses the carry fence adds one `waitForFence`. Carrying the composed frame's G-buffer batch this way, rather than on a trailing encoder that held the shadow passes' kernels back, took the frame's GPU time over the native arm's from +26 µs to −40 µs on an M3 Max, with the other overhead fixes in place (a diagnostic run of ten paired processes, [ADR 0014](../decisions/0014-overhead-measurement.md)); each fence update or wait added about one Metal allocation per frame there, three in the frame against the one encoder saved. A device that has run no counted draw sends neither: C4's, G1's and G2's frames then allocate one block fewer and C6's two, and G1's and G2's frames took about 0.1 to 0.25 µs less CPU (allocation stacks of one measured frame and eight paired processes, M3 Max, 2026-10-20) | native | native |

The Metal 4 visibility rules are conservative in one place. Selecting `Device` visibility for any
named access, where NRI selects it only when a source access writes, is a tuning point measured
under [ADR 0014](../decisions/0014-overhead-measurement.md) before it is narrowed. `ResourceAlias`
is not a tuning point but a cost: on the composed barrier workload C6 it added 32% to the GPU time
over native code when every global barrier carried it, and none when it is left off (a paired
diagnostic on an M-series Mac, 1,000 barriers per submission, unjudged). The core therefore tracks,
as placed resources are created and destroyed, which of them have overlapped the bytes of another
placed resource, one that lives or lived there, and the backend reads one relaxed load per global
barrier while recording (the aliased and sparse counts, kept in one word) and up to two more per
texture barrier from `Undefined` (a texture's flag, the counts). A resource stays aliased until it
is destroyed.

## Cross-queue handoff

A texture handed between queues is, at the handoff, in a layout both queues accept
(`VU-rojo-sync-0025`), unless the acquiring queue discards it from `Undefined` first; a discard
names no earlier layout on any backend (*unverified* on D3D12), and the copy queue cannot discard,
since a Copy list keeps a texture barrier in `General` (`VU-rojo-sync-0026`). Between the graphics
and compute queues that is `General`, `ShaderRead`, `ShaderWrite`, `CopySrc` or `CopyDst`: on D3D12
they lower to `LAYOUT_COMMON` and the generic `LAYOUT_SHADER_RESOURCE`, `LAYOUT_UNORDERED_ACCESS`,
`LAYOUT_COPY_SOURCE` and `LAYOUT_COPY_DEST`, which direct and compute queues both accept, so a
texture created with initial data in `ShaderRead` is read on either queue without a transition. A
handoff to or from the copy queue is in `General`, which maps to `D3D12_BARRIER_LAYOUT_COMMON`,
taken to be the one layout every D3D12 queue type accepts (*unverified*; see
[part 1](09-synchronization.md)). On Vulkan, resources are shared concurrently between queue
families, so no ownership transfer is needed and every layout is valid on every queue that supports
its use. Buffers need no transition. In every case the acquiring queue waits on a fence the
releasing queue signals after its last use ([queues and frames](10-queues-and-frames.md)). The
validation tracker follows layouts per queue and checks the handoff.

## First use of aliased memory

Placement ([memory and buffers](03-memory-and-buffers.md)) lets resources share memory; the policy
belongs to the caller's graph. The first use of a placed texture is a barrier from `Undefined`
(`VU-rojo-sync-0017`), which discards whatever the earlier resource left and, on D3D12,
initializes compression metadata. The first use of a placed buffer is a global barrier
(`VU-rojo-sync-0009`). Either barrier's source covers the last use of the resource that held the
memory before. A placed texture's first use after another resource used its memory is therefore not
on a Copy list, which cannot discard (`VU-rojo-sync-0026`): a Graphics or Compute list discards it,
and a fence orders that barrier before the Copy list. A tile `Queue::mapTiles` newly maps follows
the same rule (`VU-rojo-sparse-0014`, [sparse resources](13-sparse.md)).
