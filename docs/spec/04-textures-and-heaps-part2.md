# 04. Textures and heaps (part 2)

**Status**: Proposed (until spike 2 of ADR 0014 reports)
**API version**: 0.1.0

This part specifies samplers and caller-owned index ranges; textures, views and the view heap are
in [part 1](04-textures-and-heaps.md). The Metal 4 index mechanism is pending spike 2 as stated
there, and this part stays `Proposed` with it. Every `Device` function here is free-threaded and
may lock internally. Writes to distinct slots, of one range or of different ranges, may run
concurrently; two writes to the same slot, or a write racing the release of its range, may not.

## Heap index management

The default is RHI-assigned slots: [`Device::createView`](04-textures-and-heaps.md#devicecreateview)
and [`Device::createSampler`](#devicecreatesampler) allocate a slot, and
[`Device::destroy`](02-device.md#devicedestroy) releases it through the same deferred path as any
resource, so a slot is never reused while unretired work may read it.

Callers that need layout control reserve caller-owned contiguous ranges. Ranges let a material
store one base index and an offset, let an offline tool bake indices into asset data, and let
streaming replace the texture behind a stable index. A write takes effect for work submitted after
it. Replacing a slot that unretired work may still read is a data race the validation tracker
cannot see, because a bindless shader's reads are invisible to the CPU. The safe recipe is: write
into a fresh slot, publish its index, then release the old slot. The validation tracker checks what
it can see: writes into released or unreserved ranges, and slot type against use.

RHI-assigned slots are simple, safe by construction and hide backend differences such as D3D12's
separate read and write descriptors and Metal's ID tables, but give no layout control, no
deterministic indices and no in-place replacement. Caller-owned ranges match D3D12 Shader Model 6.6
heaps and `VK_EXT_descriptor_heap` exactly, but move allocation and safe reuse onto the caller. The
hybrid costs nothing when ranges are unused.

### `SamplerDesc`
```cpp
enum class SamplerFilter : uint8_t { Nearest, Linear };
enum class SamplerAddress : uint8_t { Repeat, MirrorRepeat, ClampToEdge };
struct SamplerDesc {
    SamplerFilter filter = SamplerFilter::Linear;      // minification and magnification
    SamplerFilter mipFilter = SamplerFilter::Linear;   // between mip levels
    SamplerAddress address = SamplerAddress::Repeat;   // every axis
    uint32_t maxAnisotropy = 1;                        // 1 disables anisotropic filtering
    bool compareEnable = false;                        // true: a comparison sampler
    CompareOp compare = CompareOp::LessOrEqual;        // used only when compareEnable
    std::string_view label;                            // empty: a rojorhi.-prefixed default
};
```
`CompareOp` is the one comparison enum of the interface, defined in
[pipelines](06-pipelines.md#compareop); `compare` applies only when `compareEnable` is true.
`LessOrEqual` is the shadow comparison for a conventional depth buffer and `GreaterOrEqual` its
reversed-depth counterpart. A comparison sampler is fetched only for depth comparisons.
#### Valid usage
- `VU-rojo-texture-0029` `maxAnisotropy` is between 1 and 16 inclusive.
- `VU-rojo-texture-0030` When `maxAnisotropy` is greater than 1, `filter` and `mipFilter` are
  `Linear`.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Descriptor | `MTL::SamplerDescriptor` with `setMinFilter`, `setMagFilter`, `setMipFilter`, `setSAddressMode`, `setTAddressMode`, `setRAddressMode`, `setMaxAnisotropy`, `setCompareFunction` and `setSupportArgumentBuffers(true)` | `D3D12_SAMPLER_DESC` with a `D3D12_FILTER`, including comparison and anisotropic variants, `D3D12_TEXTURE_ADDRESS_MODE` and `ComparisonFunc` | `VkSamplerCreateInfo` with `compareEnable` and `compareOp` |
| Cost | native | native | native |

### `SamplerIndex`
```cpp
enum class SamplerIndex : uint32_t {};
```
A 32-bit index into the global sampler heap, stored in shader data with `std::to_underlying`. A
shader fetches a sampler index only as a sampler of the kind in its slot, and never fetches an empty
or released slot; these shader-side requirements are stated with
[`rojoRHI::sampler`](05-shader-abi-part2.md#rojorhisampler), and no tier diagnoses them.
#### Valid usage
- None.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Fetch | sampler `MTL::ResourceID` loaded from the hidden sampler ID table | `SamplerDescriptorHeap[index]` | sampler descriptor heap index |
| Limit | 996 samplers reachable per stage on Apple7 and Apple8, 500,000 on Apple9 and later | 2,048 in the shader-visible sampler heap | sampler heap size of `VK_EXT_descriptor_heap` (value unverified) |
| Cost | cheap: one extra 8-byte load | native | native |

### `Device::createSampler`
```cpp
Result<SamplerIndex> Device::createSampler(const SamplerDesc& desc);
```
Allocates an RHI-assigned slot in the sampler heap and writes the sampler into it. The sampler
takes effect for work submitted after the call; [`Device::destroy`](02-device.md#devicedestroy)
releases the slot through the deferred path. A full sampler heap is an error result.
#### Valid usage
- None.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::Device::newSamplerState(descriptor)`, its `MTL::SamplerState::gpuResourceID()` written into the sampler ID table | `ID3D12Device::CreateSampler` at the slot's handle in the shader-visible sampler heap | `vkWriteSamplerDescriptorsEXT` into the sampler heap |
| Cost | cheap: one sampler object and one 8-byte table write | native | native |

### `IndexRange`
```cpp
struct IndexRange {
    enum class Kind : uint8_t { Views, Samplers };
    uint32_t base = 0;          // first slot of the range
    uint32_t count = 0;         // slots in the range
    Kind kind = Kind::Views;    // heap the range lives in, set by the reserving call
};
```
A caller-owned contiguous range of heap slots. Slot `base + i` for `i` below `count` is the view
index or sampler index a shader fetches, so a range is indexable by offset in shader code.
#### Valid usage
- `VU-rojo-texture-0031` An `IndexRange` passed to RojoRHI is a value returned by
  `Device::reserveViews` or `Device::reserveSamplers`, unmodified, that has not been released.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Storage | contiguous entries of the hidden ID table; view-pool slots pending spike 2 | contiguous descriptors of the shader-visible heap | contiguous slots of the resource or sampler heap |
| Cost | cheap: backend-maintained ID table | native | native |

### `Device::reserveViews`
```cpp
Result<IndexRange> Device::reserveViews(uint32_t count);
```
Reserves `count` contiguous slots in the view heap, returned with `Kind::Views`. The slots hold no
view until [`Device::writeView`](#devicewriteview) writes one. No contiguous run of `count` free
slots is an error result. The call is free-threaded.
#### Valid usage
- `VU-rojo-texture-0032` `count` is greater than zero.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | contiguous ID-table entries, no native call | contiguous run of the shader-visible `CBV_SRV_UAV` heap, no native call | contiguous run of the resource heap, no native call |
| Cost | cheap: backend-maintained ID table | native | native |

### `Device::writeView`
```cpp
void Device::writeView(IndexRange range, uint32_t index, Texture texture,
                       const TextureViewDesc& desc);
```
Writes a view of `texture` into slot `range.base + index`, replacing any view there. The write
takes effect for work submitted after the call. A native refusal to create the view is fatal. The
call is free-threaded; writes to distinct slots may run concurrently.

No submitted, unretired work reads the slot being written, and no other `Device::writeView` call
writes the same slot concurrently. Both are data races no tier diagnoses: bindless reads are
invisible to the CPU, and RojoRHI takes no per-slot lock.
#### Valid usage
- `VU-rojo-texture-0033` `range.kind` is `Views` and `index` is less than `range.count`.
- `VU-rojo-texture-0034` `desc` satisfies the
  [`TextureViewDesc`](04-textures-and-heaps.md#textureviewdesc) rules for `texture`, and `texture`
  outlives the slot as [`Device::createView`](04-textures-and-heaps.md#devicecreateview) requires.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::Texture::newTextureView` or the texture itself, `MTL::Texture::gpuResourceID()` written into the ID table; `MTL::TextureViewPool::setTextureView` pending spike 2 | `CreateShaderResourceView` or `CreateUnorderedAccessView` at the slot's handle | `vkCreateImageView`, `vkWriteResourceDescriptorsEXT` |
| Cost | cheap: one view object and one 8-byte table write | native | native |

### `Device::reserveSamplers`
```cpp
Result<IndexRange> Device::reserveSamplers(uint32_t count);
```
Reserves `count` contiguous slots in the sampler heap, returned with `Kind::Samplers`. The slots
hold no sampler until [`Device::writeSampler`](#devicewritesampler) writes one. No contiguous run
of `count` free slots is an error result. The call is free-threaded.
#### Valid usage
- `VU-rojo-texture-0035` `count` is greater than zero.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | contiguous sampler ID-table entries, no native call | contiguous run of the shader-visible sampler heap, no native call | contiguous run of the sampler heap, no native call |
| Cost | cheap: backend-maintained ID table | native | native |

### `Device::writeSampler`
```cpp
void Device::writeSampler(IndexRange range, uint32_t index, const SamplerDesc& desc);
```
Writes a sampler into slot `range.base + index`, replacing any sampler there. The write takes
effect for work submitted after the call. A native refusal to create the sampler is fatal. The
call is free-threaded; writes to distinct slots may run concurrently.

No submitted, unretired work reads the slot being written, and no other `Device::writeSampler` call
writes the same slot concurrently. As for `Device::writeView`, both are data races no tier
diagnoses.
#### Valid usage
- `VU-rojo-texture-0036` `range.kind` is `Samplers` and `index` is less than `range.count`.
- `VU-rojo-texture-0037` `desc` satisfies the [`SamplerDesc`](#samplerdesc) rules.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::Device::newSamplerState`, `MTL::SamplerState::gpuResourceID()` written into the sampler ID table | `ID3D12Device::CreateSampler` at the slot's handle | `vkWriteSamplerDescriptorsEXT` |
| Cost | cheap: one sampler object and one 8-byte table write | native | native |

### `Device::release`
```cpp
void Device::release(IndexRange range);
```
Releases a caller-owned range, deferred like `Device::destroy`: its slots and the views or
samplers in them return to the heap once the last frame that could have read them has retired.
The call is free-threaded.

No work submitted after the call fetches a slot of `range`, and no `Device::writeView` or
`Device::writeSampler` call into `range` runs concurrently with the release. No tier diagnoses
either: the fetch is a bindless shader read, and the concurrent write is a data race. A write into
`range` after the release violates the [`IndexRange`](#indexrange) rule.
#### Valid usage
- `VU-rojo-texture-0038` `range` satisfies the [`IndexRange`](#indexrange) rules; in particular it
  has not already been released.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | entries return to the ID-table allocator after retirement; view and sampler objects released | descriptors return to the heap allocator after retirement, no native call | slots return to the heap allocator after retirement, no native call |
| Cost | cheap: backend-maintained ID table | native | native |
