# 04. Textures and heaps

**Status**: Accepted
**API version**: 0.1.0

This chapter specifies textures, texture views and the global view heap. Samplers, caller-owned
index ranges and their release continue in [part 2](04-textures-and-heaps-part2.md). Placement is
defined in [memory and buffers, part 2](03-memory-and-buffers-part2.md), residency in
[memory and buffers](03-memory-and-buffers.md), extents in [fundamentals](01-fundamentals.md),
formats in [formats and limits](15-formats-and-limits.md), size limits in
[formats and limits, part 2](15-formats-and-limits-part2.md), barriers and layouts in
[synchronization](09-synchronization.md). Creation functions are free-threaded.

A texture is used directly as an attachment, as a copy endpoint and in barriers. A shader reaches
it only through a view index: views and samplers are 32-bit indices into two global heaps, stored
as `uint32` fields in roots and other shader data and fetched through the
[shader ABI](05-shader-abi.md). Heap sizes are reported by
[`Device::capabilities`](02-device.md#devicecapabilities); the D3D12 floor gives one million
resource descriptors and 2,048 samplers. A texture created without initial data starts in
`Layout::Undefined`; one created with initial data starts in `Layout::ShaderRead`, which the
graphics and compute queues both accept
([cross-queue handoff](09-synchronization-part2.md#cross-queue-handoff)). A Copy list finds a
texture created without initial data, neither placed nor `Sparse`, in `Layout::General` at its
first use or barrier there, when no use or barrier on any queue reached it first
([`Layout`](09-synchronization.md#layout)).

On Metal 4 a view index reads a CPU-written table of 8-byte `MTL::ResourceID`s that the backend
binds at a reserved argument-table slot. Spike 2 of
[ADR 0014](../decisions/0014-overhead-measurement.md) measured it against the contiguous
`base + i` resource IDs of a `MTL::TextureViewPool`, which is faster on the GPU but rests on
undocumented ID arithmetic, and kept the documented table
([spike 2](../research/2026-10-12-spike-2-texture-indexing.md)).

### `TextureType`
```cpp
enum class TextureType : uint8_t {
    Tex1D, Tex1DArray, Tex2D, Tex2DArray, Tex3D, Cube, CubeArray,
};
```
For `Cube` and `CubeArray`, array layers count faces, six per cube, in the order +X, -X, +Y, -Y,
+Z, -Z. Multisampling is a property of `TextureDesc::samples`, not a separate type.
#### Valid usage
- None.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Type | `MTL::TextureType1D`, `MTL::TextureType1DArray`, `MTL::TextureType2D`, `MTL::TextureType2DArray`, `MTL::TextureType2DMultisample`, `MTL::TextureType2DMultisampleArray`, `MTL::TextureType3D`, `MTL::TextureTypeCube`, `MTL::TextureTypeCubeArray`; array length counts cubes; a 1D type has one mip | `D3D12_RESOURCE_DIMENSION_TEXTURE1D`, `TEXTURE2D`, `TEXTURE3D`; a cube is a 2D array of 6n layers | `VK_IMAGE_TYPE_1D`, `2D`, `3D`; cubes with `VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT` |
| Cost | native | native | native |

### `TextureUsage`
```cpp
enum class TextureUsage : uint32_t {
    None         = 0,
    Sampled      = 1u << 0,   // read through a sampled view
    Storage      = 1u << 1,   // read or written through a storage view
    RenderTarget = 1u << 2,   // color attachment
    DepthStencil = 1u << 3,   // depth-stencil attachment
    Sparse       = 1u << 4,   // reserved: no backing memory until tiles are mapped
};
constexpr TextureUsage operator|(TextureUsage, TextureUsage);
constexpr TextureUsage operator&(TextureUsage, TextureUsage);
```
Every texture is a copy source and destination without a flag. Tiles of a `Sparse` texture are
mapped through [sparse resources](13-sparse.md). Sparse support is reported by
[`Device::capabilities`](02-device.md#devicecapabilities): on Metal 4 it is
`MTL::Device::supportsPlacementSparse()`, guaranteed on Apple8 and every Apple7 Mac but not on
every Apple7 GPU.
#### Valid usage
- None.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Flags | `MTL::TextureUsageShaderRead`, `MTL::TextureUsageShaderWrite`, `MTL::TextureUsageRenderTarget` | `D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS`, `ALLOW_RENDER_TARGET`, `ALLOW_DEPTH_STENCIL`; `DENY_SHADER_RESOURCE` for a depth texture without `Sampled` | `VK_IMAGE_USAGE_SAMPLED_BIT`, `STORAGE_BIT`, `COLOR_ATTACHMENT_BIT`, `DEPTH_STENCIL_ATTACHMENT_BIT`, always `TRANSFER_SRC_BIT` and `TRANSFER_DST_BIT` |
| Cost | native | native | native |

### `TextureDesc`
```cpp
struct TextureDesc {
    TextureType type = TextureType::Tex2D;   // Tex1D, Tex2D, Tex3D, Cube, and array forms
    Format format = Format::Undefined;
    Extent3D extent;                         // texels of mip 0
    uint32_t mips = 1, layers = 1, samples = 1;
    TextureUsage usage = TextureUsage::None; // Sampled | Storage | RenderTarget | DepthStencil | Sparse
    std::string_view label;                  // empty: a rojorhi.-prefixed default
};
```
`Format` is defined in [formats and limits](15-formats-and-limits.md#format) and `Extent3D` in
[fundamentals](01-fundamentals.md#extent3d). `layers` counts array layers; for cube types it counts
faces.
#### Valid usage
- `VU-rojo-texture-0001` `format` is not `Format::Undefined`.
- `VU-rojo-texture-0002` Every extent component is at least 1 and at most the dimension limit for
  `type` in [limits](15-formats-and-limits-part2.md#limits).
- `VU-rojo-texture-0003` `extent.height` is 1 for `Tex1D` and `Tex1DArray`, and `extent.depth` is 1
  for every type except `Tex3D`.
- `VU-rojo-texture-0004` `extent.width` equals `extent.height` for `Cube` and `CubeArray`.
- `VU-rojo-texture-0005` `layers` is 1 for `Tex1D`, `Tex2D` and `Tex3D`, 6 for `Cube`, a positive
  multiple of 6 for `CubeArray`, and at least 1 for the other array types.
- `VU-rojo-texture-0006` `mips` is at least 1 and at most
  floor(log2(max(width, height, depth))) + 1.
- `VU-rojo-texture-0042` `mips` is 1 for `Tex1D` and `Tex1DArray`, because Metal has no mipmapped
  one-dimensional textures.
- `VU-rojo-texture-0007` `samples` is 1, 2, 4 or 8 and is supported for `format`.
- `VU-rojo-texture-0008` When `samples` is greater than 1, `type` is `Tex2D` or `Tex2DArray`, `mips`
  is 1, and `usage` contains neither `Storage` nor `Sparse`.
- `VU-rojo-texture-0009` `usage` is not `None` and does not contain both `RenderTarget` and
  `DepthStencil`.
- `VU-rojo-texture-0010` Each usage bit is supported for `format`: `DepthStencil` only for a
  depth or stencil format, `RenderTarget` only where the format capabilities report
  `colorAttachment`, and `Storage` only where they report `storageRead` or `storageWrite`.
- `VU-rojo-texture-0011` `type` is not `Tex3D` when `usage` contains `DepthStencil`.
- `VU-rojo-texture-0012` `usage` contains `Sparse` only when `Device::capabilities` reports sparse
  resources.
- `VU-rojo-texture-0039` `layers` is at most `Limits::maxTextureLayers`; for cube types each face
  counts as a layer.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Descriptor | `MTL::TextureDescriptor` with `MTL::StorageModePrivate` | `D3D12_RESOURCE_DESC1` | `VkImageCreateInfo` with `VK_IMAGE_TILING_OPTIMAL` |
| Cost | native | native | native |

### `TextureData`
```cpp
struct TextureData {
    std::span<const std::byte> bytes;   // empty: the subresource stays undefined
    uint64_t bytesPerRow = 0;           // distance between rows of texels or compressed blocks
    uint64_t bytesPerImage = 0;         // distance between depth slices of a Tex3D mip; else 0
};
```
One `TextureData` describes one subresource of the initial contents. Rows of a block-compressed
format are rows of blocks, counted with rounding up. The caller states the strides because only the
caller knows whether its rows are padded. The stride rules apply when `bytes` is not empty. The
size `bytes` must hold is `bytesPerImage * (depth - 1)` for a `Tex3D` mip (none otherwise), plus
`bytesPerRow * (rows - 1)`, plus one packed row, where `depth` and `rows` are those of the
subresource's mip; the last row is not padded to the stride. In a `Tex3D` mip, a
`bytesPerRow * rows` product that overflows 64 bits violates `VU-rojo-texture-0014`; any other
overflow in the implied size violates `VU-rojo-texture-0015`.
#### Valid usage
- `VU-rojo-texture-0013` `bytesPerRow` is at least the packed row size of the subresource and a
  multiple of the format's texel or block size.
- `VU-rojo-texture-0014` For a `Tex3D` mip, `bytesPerImage` is at least `bytesPerRow` times the
  rows of one slice; otherwise `bytesPerImage` is 0.
- `VU-rojo-texture-0015` A non-empty `bytes` holds at least the bytes the strides imply for the
  whole subresource.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Strides | `sourceBytesPerRow` and `sourceBytesPerImage` arguments of the staged `MTL4::ComputeCommandEncoder::copyFromBuffer` | `D3D12_SUBRESOURCE_FOOTPRINT::RowPitch`; staging repacks rows to `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT` | `VkBufferImageCopy2::bufferRowLength` and `bufferImageHeight` in texels |
| Cost | native | cheap: rows are repacked to a 256-byte pitch | native |

### `SubresourceRange`
```cpp
inline constexpr uint32_t kAllMips = ~0u;
inline constexpr uint32_t kAllLayers = ~0u;
struct SubresourceRange {
    uint32_t baseMip = 0, mipCount = kAllMips;        // kAllMips: through the last mip
    uint32_t baseLayer = 0, layerCount = kAllLayers;  // kAllLayers: through the last layer
};
```
Names the mips and array layers a view or a [texture barrier](09-synchronization.md) covers. The
default covers the whole texture; layers of cube types are faces.
#### Valid usage
- `VU-rojo-texture-0016` `mipCount` and `layerCount` are not 0.
- `VU-rojo-texture-0017` `baseMip` plus `mipCount`, unless `kAllMips`, is at most the texture's
  `mips`; `baseLayer` plus `layerCount`, unless `kAllLayers`, is at most its `layers`.
- `VU-rojo-texture-0041` `baseMip` is less than the texture's `mips`, and `baseLayer` is less than
  its `layers`.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Range | `NS::Range` levels and slices of a texture view; ignored by barriers | view mip and array fields, `D3D12_BARRIER_SUBRESOURCE_RANGE` | `VkImageSubresourceRange` |
| Cost | native | native | native |

### `Device::createTexture`
```cpp
Result<Texture> Device::createTexture(const TextureDesc& desc,
                                      std::span<const TextureData> initial = {},
                                      Placement placement = {});
```
Creates a texture, labels it and adds it to the residency set. A non-null `placement.heap` creates
it inside that heap (see [`Placement`](03-memory-and-buffers-part2.md#placement)). `initial` holds
one entry per subresource, ordered by layer and then by mip within a layer (index
`mip + layer * mips`). Without initial data the texture starts in `Layout::Undefined`, and a Copy
list finds it in `Layout::General` at its first use or barrier when no use or barrier on any queue
reached it first, unless it is placed or `Sparse`. With initial data the backend stages and copies
it and transitions the texture; the copy completes and is visible before any work submitted after
the call returns, and the texture starts in `Layout::ShaderRead`. Out of memory, an unsupported
format and a native refusal are error results.
#### Valid usage
- `VU-rojo-texture-0018` `initial` is empty or has exactly `desc.mips * desc.layers` entries.
- `VU-rojo-texture-0019` `initial` is empty when `desc.usage` contains `Sparse`.
- `VU-rojo-texture-0020` `initial` is empty unless `desc.usage` contains `Sampled`.
- `VU-rojo-texture-0021` `initial` is empty when `desc.samples` is greater than 1.
- `VU-rojo-texture-0040` `initial` is empty when `desc.format` is `Format::D32FloatS8`, which has
  no packed texel size.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Allocation | `MTL::Device::newTexture(descriptor)`; placed: `MTL::Heap::newTexture(descriptor, offset)` | `ID3D12Device10::CreateCommittedResource3` with `D3D12_BARRIER_LAYOUT_COMMON`, the one layout a copy queue accesses, or `D3D12_BARRIER_LAYOUT_COPY_DEST` when `initial` is not empty; placed: `CreatePlacedResource2`; sparse: `CreateReservedResource2` | `vkCreateImage` with `VK_IMAGE_LAYOUT_UNDEFINED`, `vkAllocateMemory`, `vkBindImageMemory`; a texture without initial data that is neither placed nor sparse is then moved from `VK_IMAGE_LAYOUT_UNDEFINED` to `VK_IMAGE_LAYOUT_GENERAL` by a staged barrier, the layout a Copy list finds it in |
| Initial data | staged `MTL4::ComputeCommandEncoder::copyFromBuffer` into the texture; no layout | created in `D3D12_BARRIER_LAYOUT_COPY_DEST`; `GetCopyableFootprints`, staged `CopyTextureRegion`, then a texture barrier from `D3D12_BARRIER_LAYOUT_COPY_DEST` to the generic `D3D12_BARRIER_LAYOUT_SHADER_RESOURCE`, which direct and compute queues both accept | staged `vkCmdCopyBufferToImage2`, then `VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL` to `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL` |
| Cost | native | native | native |

### `TextureViewDesc`
```cpp
enum class TextureAspect : uint8_t { Color, Depth, Stencil };
struct TextureViewDesc {
    TextureType type = TextureType::Tex2D;         // dimensionality the shader fetches
    Format format = Format::Undefined;             // Undefined: the texture's own format
    SubresourceRange range;                        // mips and layers exposed
    TextureUsage usage = TextureUsage::Sampled;    // exactly Sampled or Storage
    TextureAspect aspect = TextureAspect::Color;   // plane of a depth-stencil format
};
```
Describes how a shader sees a texture through a view index. `aspect` selects the depth or the
stencil plane of a depth-stencil format, so the stencil of `D32FloatS8` can be sampled. `format`
may reinterpret only between the sRGB and linear members of one format family; the backend sets the
castable or mutable-format flags at creation whenever a native API needs them for a texture's
format family, so `TextureDesc` carries no view-format list.
#### Valid usage
- `VU-rojo-texture-0022` `usage` is exactly `Sampled` or exactly `Storage`, and the texture was
  created with that usage.
- `VU-rojo-texture-0023` `type` is compatible with the texture's type: a `Tex1D` or `Tex1DArray`
  view of a `Tex1D` or `Tex1DArray` texture; a `Tex2D` or `Tex2DArray` view of a `Tex2D`,
  `Tex2DArray`, `Cube` or `CubeArray` texture; a `Cube` view of a `Cube` or `CubeArray` texture; a
  `CubeArray` view of a `CubeArray` texture; a `Tex3D` view of a `Tex3D` texture.
- `VU-rojo-texture-0024` A `Tex1D`, `Tex2D` or `Tex3D` view covers one layer, a `Cube` view six
  faces, and a `CubeArray` view a multiple of six faces.
- `VU-rojo-texture-0025` `format` is `Undefined`, the texture's format, or that format's sRGB or
  linear counterpart.
- `VU-rojo-texture-0026` A `Storage` view covers exactly one mip, and its texture has `samples` 1.
- `VU-rojo-texture-0027` `aspect` is `Color` for a color format. For a depth format, `aspect` is
  `Depth` with the texture's format `D32Float` or `D32FloatS8`, or `Stencil` with `D32FloatS8`
  only; `format` is then `Undefined` or the texture's own format, and a `Stencil` view is
  `Sampled`.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Descriptor | `MTL::TextureViewDescriptor`; an sRGB or linear partner view needs no flag, and the stencil plane of a `Sampled` `D32FloatS8` texture needs `MTL::TextureUsagePixelFormatView` at creation | `D3D12_SHADER_RESOURCE_VIEW_DESC` or `D3D12_UNORDERED_ACCESS_VIEW_DESC`; reinterpretation through castable formats at creation | `VkImageViewCreateInfo`; reinterpretation through `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` and a format list |
| Aspect | stencil through an `MTL::PixelFormatX32_Stencil8` view | plane slice 1 with `DXGI_FORMAT_X32_TYPELESS_G8X24_UINT` | `VK_IMAGE_ASPECT_DEPTH_BIT` or `VK_IMAGE_ASPECT_STENCIL_BIT` |
| Cost | native | native | native |

### `Device::createView`
```cpp
Result<ViewIndex> Device::createView(Texture texture, const TextureViewDesc& desc);
```
Allocates an RHI-assigned slot in the view heap and writes a view of `texture` into it. The view
takes effect for work submitted after the call. [`Device::destroy`](02-device.md#devicedestroy)
releases the slot through the deferred path, so a slot is never reused while unretired work may
read it. A full view heap is an error result.
#### Valid usage
- `VU-rojo-texture-0028` `texture` is destroyed only after every slot holding a view of it has been
  destroyed, released or rewritten.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::Texture::newTextureView(format, type, levels, slices)` or the texture itself, its `MTL::Texture::gpuResourceID()` written into the ID table | `ID3D12Device::CreateShaderResourceView` or `CreateUnorderedAccessView` at the slot's handle in the shader-visible `CBV_SRV_UAV` heap | `vkCreateImageView`, `vkWriteResourceDescriptorsEXT` into the resource heap |
| Cost | cheap: one view object and one 8-byte table write | native | native |

### `ViewIndex`
```cpp
enum class ViewIndex : uint32_t {};
```
A 32-bit index into the global view heap, stored in shader data with `std::to_underlying`. An
index taken from an [`IndexRange`](04-textures-and-heaps-part2.md#indexrange) is its `base` plus
an offset below its `count`. A shader fetches a view index only as the type and access of the view
in its slot, and never fetches an empty or released slot; these shader-side requirements are stated
with [`rojoRHI::texture`](05-shader-abi-part2.md#rojorhitexture), and no tier diagnoses them.
#### Valid usage
- None.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Fetch | 8-byte `MTL::ResourceID` loaded from the hidden ID table | `ResourceDescriptorHeap[index]` | resource descriptor heap index |
| Cost | cheap: one extra 8-byte load, measured at about 3% for `read` and 7.5% for `sample` in a fetch-only kernel | native | native |
