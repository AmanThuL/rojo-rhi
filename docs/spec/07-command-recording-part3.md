# 07. Command recording: copies and clears

**Status**: Accepted
**API version**: 0.1.0

This part of the command-recording chapter ([command recording](07-command-recording.md)) defines
copies, fills and clears. All are recorded outside render passes; on Metal 4 copies and fills
record on the lazily opened compute encoder. For barriers a copy reads and writes with the copy
stage and copy accesses, and `fillBuffer` and `clearTexture` count as copy writes, a cleared
texture being in `Layout::CopyDst` or `Layout::General` ([synchronization](09-synchronization.md));
copies are not ordered against each other or against dispatches without a barrier.

**Copies take buffer handles and offsets, not addresses.** Metal 4 copy commands take the native
`MTL::Buffer`, and recovering it from an `Addr`, a GPU virtual address there, means a reverse lookup
from address to buffer, the structure that capped No Graphics API's Metal backend at 64 memory
blocks. D3D12's `CopyBufferRegion` likewise takes the `ID3D12Resource`. Indirect arguments take
addresses instead ([GPU-driven rendering](08-gpu-driven.md)), because Metal 4 consumes them by GPU
address and the D3D12 form carries the heap index its resource is looked up from.

Buffer and texture copy layouts follow the strictest backend: D3D12 places buffer-side texture data
at 512-byte offsets with 256-byte row pitches, unless an optional cap that RojoRHI does not assume
relaxes it. Copies of formats with a stencil component are not part of 0.1.0.

### `BufferTextureLayout`

```cpp
struct BufferTextureLayout {
    uint64_t offset = 0;        // bytes from the buffer start to the first texel block
    uint32_t rowPitch = 0;      // bytes between rows of texel blocks
    uint32_t rowsPerImage = 0;  // rows of blocks between depth slices and layers; 0: region height
};
```

How texel data is laid out in a buffer for `copyBufferToTexture` and `copyTextureToBuffer`. Depth
slices follow each other, then layers, each `rowPitch * rowsPerImage` bytes apart.

#### Valid usage

- `VU-rojo-cmd-0034` `offset` is a multiple of 512.
- `VU-rojo-cmd-0035` `rowPitch` is a multiple of 256 and at least the byte width of one row of the
  region's texel blocks.
- `VU-rojo-cmd-0036` `rowsPerImage` is zero or at least the region's height in texel blocks.
- `VU-rojo-cmd-0037` When the region spans more than one depth slice or layer, the byte distance
  between consecutive images is a multiple of 512.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | bytes-per-row and bytes-per-image arguments of the `MTL4::ComputeCommandEncoder` copies | `D3D12_PLACED_SUBRESOURCE_FOOTPRINT` (`Offset`, `RowPitch`, `Height`) | `VkBufferImageCopy2` (`bufferOffset`, `bufferRowLength`, `bufferImageHeight` in texels) |
| Cost | native | native | native |

### `TextureRegion`

```cpp
struct TextureRegion {
    uint32_t mip = 0, layer = 0, layers = 1;
    Offset3D offset; Extent3D extent;   // texels
};
```

A box of texels in one mip of a range of array layers. `Offset3D` and `Extent3D` are defined in
[fundamentals](01-fundamentals.md#offset3d).

#### Valid usage

- `VU-rojo-cmd-0038` `mip` is below the texture's mip count, `layers` is at least one and
  `layer + layers` is at most its layer count.
- `VU-rojo-cmd-0039` `extent` is non-zero and `offset + extent` lies within the mip's extent.
- `VU-rojo-cmd-0040` For block-compressed formats, `offset` is a multiple of the block size and
  `extent` is a multiple of it or reaches the mip's edge.
- `VU-rojo-cmd-0041` The texture's format has no stencil component.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | slice, level, origin and size arguments | `D3D12_TEXTURE_COPY_LOCATION` subresource index and `D3D12_BOX` | `VkImageSubresourceLayers`, offset and extent |
| Cost | native | native | native |

### `CommandList::copyBuffer`

```cpp
void CommandList::copyBuffer(Buffer src, uint64_t srcOffset, Buffer dst, uint64_t dstOffset,
                             uint64_t size);
```

Copies `size` bytes from `src` at `srcOffset` to `dst` at `dstOffset`.

#### Valid usage

- `VU-rojo-cmd-0042` Recorded outside a render pass.
- `VU-rojo-cmd-0043` `size` is greater than zero, both ranges lie within their buffers, and
  `srcOffset`, `dstOffset` and `size` are multiples of 4.
- `VU-rojo-cmd-0044` When `src` and `dst` are the same buffer, the ranges do not overlap.
- `VU-rojo-cmd-0045` Retired in 0.1.0: `src` is not `Memory::Readback` and `dst` is not
  `Memory::Upload`. `VU-rojo-buffer-0001` and `VU-rojo-buffer-0002` state the copy roles of every
  buffer once, for every copy command ([memory and buffers](03-memory-and-buffers.md)).

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::ComputeCommandEncoder::copyFromBuffer` (buffer to buffer) | `CopyBufferRegion` | `vkCmdCopyBuffer2` |
| Cost | native | native | native |

### `CommandList::copyBufferToTexture`

```cpp
void CommandList::copyBufferToTexture(Buffer src, const BufferTextureLayout& layout, Texture dst,
                                      const TextureRegion& region);
```

Copies texel data laid out by `layout` in `src` into `region` of `dst`.

#### Valid usage

- `VU-rojo-cmd-0046` Recorded outside a render pass.
- `VU-rojo-cmd-0047` Retired in 0.1.0: The bytes `layout` addresses for `region` lie within `src`,
  and `src` is not `Memory::Readback`. The bounds are `VU-rojo-cmd-0064`; the memory class is
  `VU-rojo-buffer-0002`.
- `VU-rojo-cmd-0064` The bytes `layout` addresses for `region` lie within `src`.
- `VU-rojo-cmd-0048` Retired in 0.1.0: `dst` has one sample, and the region's subresources are in
  `Layout::CopyDst` or `Layout::General`. The sample count is `VU-rojo-cmd-0066`, a Debug-tier
  rule; the layouts are `VU-rojo-cmd-0067`.
- `VU-rojo-cmd-0066` `dst` has one sample.
- `VU-rojo-cmd-0067` The region's subresources are in `Layout::CopyDst` or `Layout::General`.
  Checked by the validation tracker.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::ComputeCommandEncoder::copyFromBuffer` (buffer to texture), one call per layer, or one over the depth of a 3D texture | `CopyTextureRegion` from a placed footprint, one call per layer | `vkCmdCopyBufferToImage2` |
| Cost | native | native | native |

### `CommandList::copyTextureToBuffer`

```cpp
void CommandList::copyTextureToBuffer(Texture src, const TextureRegion& region, Buffer dst,
                                      const BufferTextureLayout& layout);
```

Copies `region` of `src` into `dst`, laid out by `layout`.

#### Valid usage

- `VU-rojo-cmd-0049` Recorded outside a render pass.
- `VU-rojo-cmd-0050` Retired in 0.1.0: The bytes `layout` addresses for `region` lie within `dst`,
  and `dst` is not `Memory::Upload`. The bounds are `VU-rojo-cmd-0065`; the memory class is
  `VU-rojo-buffer-0001`.
- `VU-rojo-cmd-0065` The bytes `layout` addresses for `region` lie within `dst`.
- `VU-rojo-cmd-0051` Retired in 0.1.0: `src` has one sample, and the region's subresources are in
  `Layout::CopySrc` or `Layout::General`. The sample count is `VU-rojo-cmd-0068`, a Debug-tier
  rule; the layouts are `VU-rojo-cmd-0069`.
- `VU-rojo-cmd-0068` `src` has one sample.
- `VU-rojo-cmd-0069` The region's subresources are in `Layout::CopySrc` or `Layout::General`.
  Checked by the validation tracker.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::ComputeCommandEncoder::copyFromTexture` (texture to buffer), one call per layer, or one over the depth of a 3D texture | `CopyTextureRegion` to a placed footprint, one call per layer | `vkCmdCopyImageToBuffer2` |
| Cost | native | native | native |

### `CommandList::copyTexture`

```cpp
void CommandList::copyTexture(Texture src, const TextureRegion& srcRegion, Texture dst,
                              const TextureRegion& dstRegion);
```

Copies texels between textures of the same format, or of two formats that differ only in their
sRGB transfer, which [formats and limits](15-formats-and-limits.md#format) defines as
copy-compatible: the stored bits move unchanged.

#### Valid usage

- `VU-rojo-cmd-0052` Recorded outside a render pass.
- `VU-rojo-cmd-0053` The formats are equal or differ only in sRGB transfer, both textures have one
  sample, and the regions have equal extents in texel blocks and equal layer counts.
- `VU-rojo-cmd-0054` The source subresources are in `Layout::CopySrc` or `Layout::General`, and the
  destination subresources in `Layout::CopyDst` or `Layout::General`. Checked by the validation
  tracker.
- `VU-rojo-cmd-0055` When `src` and `dst` are the same texture, the regions share no subresource.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::ComputeCommandEncoder::copyFromTexture` (texture to texture), one call per layer, or one over the depth when either texture is 3D; the size counts whole blocks | `CopyTextureRegion` | `vkCmdCopyImage2` |
| Cost | native | native | native |

### `CommandList::fillBuffer`

```cpp
void CommandList::fillBuffer(Buffer dst, uint64_t offset, uint64_t size, uint32_t pattern);
```

Writes `pattern`, repeated in little-endian byte order, over `size` bytes of `dst` from `offset`.
On D3D12 the planned backend runs its own compute kernel, because `ClearUnorderedAccessViewUint`
needs descriptor handles; that is why `dst` needs `ShaderWrite` and why fills are invalid on
`Copy` lists, which cannot dispatch on D3D12.

#### Valid usage

- `VU-rojo-cmd-0056` Recorded outside a render pass on a `Graphics` or `Compute` list.
- `VU-rojo-cmd-0057` `size` is greater than zero, `offset` and `size` are multiples of 4, and the
  range lies within `dst`.
- `VU-rojo-cmd-0058` `dst` was created with `BufferUsage::ShaderWrite`.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::ComputeCommandEncoder::fillBuffer` when the four pattern bytes are equal, else the backend's fill kernel, one thread per word | backend compute kernel through the buffer's raw unordered-access descriptor | `vkCmdFillBuffer` |
| Cost | cheap: a one-byte fill value, else one dispatch per 2^31 words on the open encoder | costly: one dispatch, pipeline and root restored after it, copy barriers widened to compute | native |

### `ClearValue`

```cpp
struct ClearValue { ClearColor color; float depth = 0.0f; uint8_t stencil = 0; };
```

The value `clearTexture` writes: `color` for color formats, `depth` and `stencil` for depth and
depth-stencil formats. `ClearColor` is defined with `ColorAttachment`
([command recording](07-command-recording.md)).

#### Valid usage

- `VU-rojo-cmd-0059` `depth` lies in `[0, 1]`.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::ClearColor`, clear depth and clear stencil of the attachment descriptors | `ColorRGBA`, `Depth`, `Stencil` arguments | `VkClearColorValue`, `VkClearDepthStencilValue` |
| Cost | native | native | native |

### `CommandList::clearTexture`

```cpp
void CommandList::clearTexture(Texture texture, const SubresourceRange& range,
                               const ClearValue& value);
```

Clears every subresource in `range` ([textures and heaps](04-textures-and-heaps.md)) outside a
render pass; inside one, clearing is a load operation (`LoadOp::Clear`). Like a copy, a clear is a
copy-stage write to subresources in a copy destination layout; the backend performs whatever
native transitions its clear needs and returns the subresources to that layout.

#### Valid usage

- `VU-rojo-cmd-0060` Recorded outside a render pass on a `Graphics` or `Compute` list.
- `VU-rojo-cmd-0061` `texture` was created with `RenderTarget`, `DepthStencil` or `Storage` usage;
  on a `Compute` list, with `Storage` and neither of the others.
- `VU-rojo-cmd-0062` Retired in 0.1.0: `range` lies within `texture`, and its subresources are in
  `Layout::CopyDst` or `Layout::General`. The range is `VU-rojo-texture-0016`, `0017` and `0041`
  ([textures and heaps](04-textures-and-heaps.md)); the layouts are `VU-rojo-cmd-0070`.
- `VU-rojo-cmd-0070` The subresources of `range` are in `Layout::CopyDst` or `Layout::General`.
  Checked by the validation tracker.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | layouts ignored; per mip and layer, and per depth plane of each mip of a 3D texture, a render encoder from `MTL4::CommandBuffer::renderCommandEncoder` with `MTL::LoadActionClear` and `MTL::StoreActionStore`, or for `Storage`-only textures one dispatch of the backend's clear kernel per mip | barriers `COPY_DEST` to `RENDER_TARGET`, `DEPTH_STENCIL_WRITE` or `UNORDERED_ACCESS` and back around `ClearRenderTargetView`, `ClearDepthStencilView` or `ClearUnorderedAccessView*` per subresource; the last needs backend-written GPU and CPU descriptors | `vkCmdClearColorImage` or `vkCmdClearDepthStencilImage` in `TRANSFER_DST_OPTIMAL` or `GENERAL` |
| Cost | costly: one render encoder per mip and layer, per depth plane for a 3D texture, closing the compute encoder; or one dispatch per mip on the compute encoder | costly: two hidden layout transitions per subresource and transient descriptors | native |
