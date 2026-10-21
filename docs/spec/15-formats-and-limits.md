# 15. Formats and limits

**Status**: Accepted
**API version**: 0.1.0

This chapter defines the texture formats the interface names and how a device reports what it can
do with each of them; the numeric limits every other chapter checks against are in
[part 2](15-formats-and-limits-part2.md). Notation, cost classes and valid-usage identifiers follow
[conventions](00-conventions.md). Values marked *unverified* come from vendor documentation or
research notes and have not been measured on hardware; a backend reports what its device actually
supports, and the reference value changes only through a new API version.

## Formats

### `Format`

```cpp
enum class Format : uint32_t {
    Undefined,        // no format
    BGRA8Unorm,       // 8-bit normalized BGRA color
    RGBA8Unorm,       // 8-bit normalized RGBA color
    RGBA8Unorm_sRGB,  // 8-bit RGBA color, sRGB transfer
    RGBA16Float,      // half-precision RGBA, the scene-linear color format
    RG16Float,        // half-precision two-channel data
    R8Unorm,          // 8-bit normalized single channel
    BC1Unorm,         // BC1 block-compressed linear color
    BC1Unorm_sRGB,    // BC1 block-compressed color, sRGB transfer
    D32Float,         // 32-bit floating-point depth
    R16Float,         // half-precision single channel
    R32Float,         // full-precision single channel
    D32FloatS8,       // 32-bit floating-point depth with 8-bit stencil
    RGB10A2Unorm,     // 10-bit normalized RGB with 2-bit alpha, the HDR10 presentation format
};
```

`Format` names the storage layout and transfer function of a texture, a texture view, an attachment
or a pipeline target. The set is the one the frozen v1 interface names, with v1's `Unknown` renamed
`Undefined`, plus `D32FloatS8` so that the stencil state and operations of the interface have a
format to act on, and `RGB10A2Unorm` so that the `HDR10` color space of
[presentation](11-presentation.md) has a format. New formats are appended in later API versions;
existing enumerator values never change
([ADR 0022](../decisions/0022-versioning-in-the-single-header.md)).

| `Format` | Texel or block | Family | `MTL::PixelFormat` | `DXGI_FORMAT` | `VkFormat` |
|---|---|---|---|---|---|
| `Undefined` | none | none | `Invalid` | `UNKNOWN` | `VK_FORMAT_UNDEFINED` |
| `BGRA8Unorm` | 4 B | own | `BGRA8Unorm` | `B8G8R8A8_UNORM` | `VK_FORMAT_B8G8R8A8_UNORM` |
| `RGBA8Unorm` | 4 B | RGBA8 | `RGBA8Unorm` | `R8G8B8A8_UNORM` | `VK_FORMAT_R8G8B8A8_UNORM` |
| `RGBA8Unorm_sRGB` | 4 B | RGBA8 | `RGBA8Unorm_sRGB` | `R8G8B8A8_UNORM_SRGB` | `VK_FORMAT_R8G8B8A8_SRGB` |
| `RGBA16Float` | 8 B | own | `RGBA16Float` | `R16G16B16A16_FLOAT` | `VK_FORMAT_R16G16B16A16_SFLOAT` |
| `RG16Float` | 4 B | own | `RG16Float` | `R16G16_FLOAT` | `VK_FORMAT_R16G16_SFLOAT` |
| `R8Unorm` | 1 B | own | `R8Unorm` | `R8_UNORM` | `VK_FORMAT_R8_UNORM` |
| `BC1Unorm` | 8 B per 4x4 block | BC1 | `BC1_RGBA` | `BC1_UNORM` | `VK_FORMAT_BC1_RGBA_UNORM_BLOCK` |
| `BC1Unorm_sRGB` | 8 B per 4x4 block | BC1 | `BC1_RGBA_sRGB` | `BC1_UNORM_SRGB` | `VK_FORMAT_BC1_RGBA_SRGB_BLOCK` |
| `D32Float` | 4 B | own | `Depth32Float` | `D32_FLOAT` | `VK_FORMAT_D32_SFLOAT` |
| `R16Float` | 2 B | own | `R16Float` | `R16_FLOAT` | `VK_FORMAT_R16_SFLOAT` |
| `R32Float` | 4 B | own | `R32Float` | `R32_FLOAT` | `VK_FORMAT_R32_SFLOAT` |
| `D32FloatS8` | 8 B on D3D12; implementation-defined on Metal 4 and Vulkan | own | `Depth32Float_Stencil8` | `D32_FLOAT_S8X24_UINT` | `VK_FORMAT_D32_SFLOAT_S8_UINT` |
| `RGB10A2Unorm` | 4 B | own | `RGB10A2Unorm` | `R10G10B10A2_UNORM` | `VK_FORMAT_A2B10G10R10_UNORM_PACK32` |

Metal names omit the `MTL::PixelFormat` prefix and DXGI names the `DXGI_FORMAT_` prefix. Every
format has a native equivalent on all three APIs; none is emulated.

**Families.** Two formats share a family when they hold the same bits and differ only in the
transfer function applied on access. A view may reinterpret a texture only within its family; the
rule and its identifier belong to `TextureViewDesc` in
[textures and heaps](04-textures-and-heaps.md). Metal 4 views an sRGB texture as its linear partner
and the reverse without a creation flag, which saved 36 microseconds of one composed frame (median
of eight paired processes, M3 Max); the backend sets `MTL::TextureUsagePixelFormatView` only for the
stencil plane view of a `Sampled` `D32FloatS8` texture, the one reinterpretation Metal's validation
layer rejects without it. Vulkan needs `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT`, and the D3D12 backend
creates the resource with a castable-format list or the family's typeless format
(`R8G8B8A8_TYPELESS`, `BC1_TYPELESS`). The backend chooses this at creation; the caller states
nothing extra.

**sRGB formats.** `RGBA8Unorm_sRGB` and `BC1Unorm_sRGB` decode to linear values when sampled, encode
on attachment writes and blend in linear space. Shader storage access to an sRGB format is not part
of the floor below. Two formats that differ only in sRGB transfer are copy-compatible: a copy
between them ([command recording](07-command-recording-part3.md#commandlistcopytexture)) moves the
stored bits unchanged and neither encodes nor decodes. `BGRA8Unorm` has no sRGB partner in this
version; presentation formats and their color spaces belong to [presentation](11-presentation.md). A
D3D12 flip-model swap chain cannot hold an `_SRGB` format, so sRGB presentation on the D3D12
backend would render through an sRGB view of a linear buffer.

**Depth formats.** `D32Float` holds depth only; `D32FloatS8` adds an 8-bit stencil, which the
stencil state in [pipelines](06-pipelines.md) and the stencil load and store operations in
[command recording](07-command-recording.md) act on. Both are depth attachments and, sampled, a
single-channel float depth read through `rojoRHI::texture`. The memory size of `D32FloatS8` is 8
bytes per texel on D3D12 and implementation-defined on Metal 4 and Vulkan, which may store the
two aspects separately; no copy layout may assume a packed size for it. The D3D12 backend creates
a sampled depth texture typeless and views it per aspect:

| `Format` | D3D12 resource | Depth-stencil view | Depth shader view | Stencil shader view |
|---|---|---|---|---|
| `D32Float` | `R32_TYPELESS` | `D32_FLOAT` | `R32_FLOAT` | none |
| `D32FloatS8` | `R32G8X24_TYPELESS` | `D32_FLOAT_S8X24_UINT` | `R32_FLOAT_X8X24_TYPELESS` | `X32_TYPELESS_G8X24_UINT` |

Stencil values are sampled through a stencil-aspect view: `X32_Stencil8` on Metal 4, a view with
`VK_IMAGE_ASPECT_STENCIL_BIT` on Vulkan and the stencil shader view above on D3D12; the view
itself belongs to [textures and heaps](04-textures-and-heaps.md). The caller sees one format.

**Block-compressed formats.** BC1 stores 4x4 texel blocks of 8 bytes. Metal's `BC1_RGBA` covers
both the opaque and the one-bit-alpha encodings, selected per block by endpoint order; Vulkan's
`BC1_RGBA` forms match it. BC formats need `supportsBCTextureCompression` on Metal 4, which every
Apple silicon Mac reports (*unverified* on Apple7 hardware by this project) and `createDevice`
([device](02-device.md)) requires, are required at
the D3D12 floor, and need the `textureCompressionBC` feature on Vulkan. Upload and copy
row pitches count rows of blocks, `((width + 3) / 4) * 8` bytes per row; see
[command recording](07-command-recording.md). A copy region in a block-compressed texture starts on
a 4-texel boundary and spans whole blocks unless it reaches the edge of its mip level, as
`VU-rojo-cmd-0040` of [`TextureRegion`](07-command-recording-part3.md#textureregion) requires.

**Guaranteed capabilities.** Every device that meets the hardware floor supports at least the
following; anything outside it is reported by `Device::formatCapabilities`. The table is read from
the Apple7 pixel-format capability tables and the D3D12 required format support at feature level
12_2 (the floor of [ADR 0010](../decisions/0010-hardware-floor.md)). Every row is checked on Metal 4
hardware by `Tests/Conformance/FormatConformanceTests.cpp`, which exercises each capability a device
reports for each format; on D3D12 it runs on WARP only, not on hardware
([evidence](../research/2026-10-21-d3d12-warp-evidence.md)). A "no" is unsupported on every backend
in this version; "reported" varies by device.

| `Format` | Sampled | Linear filter | Storage read | Storage write | Color attachment and blend | Depth attachment |
|---|---|---|---|---|---|---|
| `BGRA8Unorm` | yes | yes | reported | reported | yes | no |
| `RGBA8Unorm` | yes | yes | yes | yes | yes | no |
| `RGBA8Unorm_sRGB` | yes | yes | no | no | yes | no |
| `RGBA16Float` | yes | yes | yes | yes | yes | no |
| `RG16Float` | yes | yes | reported | yes | yes | no |
| `R8Unorm` | yes | yes | yes | yes | yes | no |
| `BC1Unorm`, `BC1Unorm_sRGB` | yes | yes | no | no | no | no |
| `D32Float` | yes | reported | no | no | no | yes |
| `R16Float` | yes | yes | yes | yes | yes | no |
| `R32Float` | yes | reported | yes | yes | reported | no |
| `D32FloatS8` | yes, depth aspect | reported | no | no | no | yes |
| `RGB10A2Unorm` | yes | yes | reported | reported | yes | no |

Linear filtering of `R32Float` on Metal 4 follows `supports32BitFloatFiltering`, and filtering of
depth formats follows the device family (*unverified*). The storage reads of `RGBA8Unorm`,
`RGBA16Float`, `R16Float` and `R8Unorm` are "yes" because the floor requires D3D12's
`TypedUAVLoadAdditionalFormats` capability, which loads that set together, and Metal 4 read-write
texture tier 2; `createDevice` ([device](02-device.md)) checks both. Without them only `R32Float`
loads would be unconditional. `RG16Float` and `RGB10A2Unorm` are outside both sets and are queried
on their own, so their storage reads are reported. Storage writes of `RG16Float` are guaranteed
while its reads are reported; a format that reports `storageWrite` without `storageRead` is written
through the write-only accessors (`rojoRHI::wTexture*`, [shader ABI](05-shader-abi-part2.md)).
Metal 4 reports exactly that for `BGRA8Unorm`, `RGB10A2Unorm` and `RG16Float`. On Vulkan
`R16Float`, `RG16Float` and `R8Unorm` storage needs `shaderStorageImageExtendedFormats`.
Multisampling, resolve and sparse residency are reported per device and are not part of the floor.

#### Valid usage
- `VU-rojo-format-0001` Every `Format` value passed to RojoRHI is one of the enumerators above.
- `VU-rojo-format-0002` `Format::Undefined` appears only in a field whose entry defines it as
  "no format" or "the texture's own format".
- `VU-rojo-format-0003` A format is used for a capability (sampling, linear filtering, storage
  read or write, color attachment, blending, depth attachment, resolve, a sample count, sparse
  residency) only when `Device::formatCapabilities` reports that capability for it.
- `VU-rojo-format-0004` A texture with a block-compressed format has a level-0 width and height
  that are multiples of 4.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Type | `MTL::PixelFormat` | `DXGI_FORMAT` | `VkFormat` |
| Family reinterpretation | none for the sRGB and linear partners; `MTL::TextureUsagePixelFormatView` for the stencil plane view | castable-format list or typeless resource format | `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` |
| Sampled depth | depth texture read directly; `X32_Stencil8` view for stencil | typeless resource with per-aspect views | depth image read directly; aspect view for stencil |
| Block-size rule | not required by Metal | required for BC level 0 | not required by Vulkan |
| Cost | native | native: typeless resources chosen at creation | native |

VU-rojo-format-0004 is the D3D12 rule for BC resources; Metal 4 and Vulkan accept any extent, and
the rule binds every backend so a texture valid on one is valid on all.

## Format capabilities

### `FormatCapabilities`

```cpp
struct FormatCapabilities {
    bool sampled = false;          // readable through a Sampled view
    bool filterLinear = false;     // linear minification and magnification filtering
    bool storageRead = false;      // readable through a Storage view
    bool storageWrite = false;     // writable through a Storage view
    bool colorAttachment = false;  // usable as a ColorAttachment texture
    bool blend = false;            // blendable as a color attachment
    bool depthAttachment = false;  // usable as a DepthAttachment texture
    bool resolve = false;          // usable as a ColorAttachment resolve target
    bool sparse = false;           // creatable with TextureUsage::Sparse
    uint32_t sampleCounts = 1;     // bit i set: 1 << i samples supported; bit 0 always set
};
```

`FormatCapabilities` states what one device supports for one format. A member is `true` only when
every operation it names is supported for every texture type the device can create in that
format. `sampleCounts` lists supported sample counts for attachments; a count missing from it is
invalid in `TextureDesc::samples` for that format. Every value is at least the guaranteed
capability in the `Format` entry above. Capabilities are fixed for the lifetime of the device.

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Source | Apple7+ feature set tables keyed by `supportsFamily`, refined by `supports32BitFloatFiltering` and `supportsTextureSampleCount`; `supportsBCTextureCompression` is part of the floor `createDevice` checks | `D3D12_FEATURE_FORMAT_SUPPORT` (`Support1`, `Support2`) | `VkFormatProperties3` optimal tiling features |
| Sample counts | `supportsTextureSampleCount` per count | `D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS` per count | `vkGetPhysicalDeviceImageFormatProperties2` |
| Sparse | `supportsPlacementSparse` and a nonzero `sparseTileSize` of the 2D type at `MTL::SparsePageSize64` | `D3D12_FORMAT_SUPPORT2_TILED`; a format with a stencil plane is never sparse: its stencil plane is a separate set of subresources that a `TileMapping` cannot name | `vkGetPhysicalDeviceSparseImageFormatProperties2` |
| Cost | cheap: backend-owned table, no per-format native query | native | native |

### `Device::formatCapabilities`

```cpp
FormatCapabilities Device::formatCapabilities(Format format) const;
```

Returns the capabilities of `format` on this device. The backend computes every format's
capabilities once at device creation, so the call is a table lookup that takes no lock and may be
made from any thread; [fundamentals](01-fundamentals.md) owns the threading contract. Whole-device
facts, such as upload-heap availability and upscalers, belong to `Capabilities` in
[device](02-device.md).

#### Valid usage
- `VU-rojo-format-0005` `format` is not `Format::Undefined`.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| At device creation | evaluate the family table for every format | `CheckFeatureSupport` for every format | `vkGetPhysicalDeviceFormatProperties2` for every format |
| Per call | array lookup | array lookup | array lookup |
| Cost | cheap: lookup in a backend-built table | native | native |
