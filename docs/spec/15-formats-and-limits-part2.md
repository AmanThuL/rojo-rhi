# 15. Formats and limits, part 2

**Status**: Accepted
**API version**: 0.1.0

This part defines the numeric limits every other chapter checks against; formats and format
capabilities are in [part 1](15-formats-and-limits.md). Notation, cost classes and valid-usage
identifiers follow [conventions](00-conventions.md). Values marked *unverified* come from vendor
documentation or research notes and have not been measured on hardware.

## Limits

### `Limits`

```cpp
struct Limits {
    uint32_t maxViews;                     // ViewIndex slots in the resource heap
    uint32_t maxSamplers;                  // SamplerIndex slots in the sampler heap
    uint32_t maxRootBlockSize;             // bytes of one root block
    uint32_t rootAlignment;                // byte alignment of a root block address
    uint32_t frameDataAlignment;           // default uploadFrameData alignment
    uint64_t maxBufferSize;                // bytes of one buffer
    uint64_t maxAddrOffset;                // bytes an Addr reaches from its buffer's start
    uint32_t maxTextureDimension1D;        // texels
    uint32_t maxTextureDimension2D;        // texels, width and height
    uint32_t maxTextureDimension3D;        // texels, each axis
    uint32_t maxTextureDimensionCube;      // texels, face width and height
    uint32_t maxTextureLayers;             // array layers, six per cube
    uint32_t maxColorAttachments;          // ColorAttachment entries in one render pass
    uint32_t maxColorBytesPerSample;       // summed attachment storage per sample
    uint32_t maxSamplerAnisotropy;         // SamplerDesc anisotropy
    uint32_t maxDrawIndirectCount;         // maxCount of one indirect draw call
    uint32_t maxDispatchGroups;            // threadgroups per dispatch dimension
    uint32_t maxThreadsPerGroup;           // threads in one compute threadgroup
    uint32_t maxGroupSize[3];              // threads per threadgroup axis, x, y, z
    uint32_t maxGroupSharedMemory;         // bytes of threadgroup (groupshared) memory
    uint32_t maxMeshTaskGroups;            // mesh task groups per dimension
    uint32_t maxMeshTaskGroupsTotal;       // product of the three mesh task dimensions
};
```

`Limits` is reported as `Capabilities::limits` by `Device::capabilities` in
[device](02-device.md) and is fixed for the lifetime of the device. Every value is at least the
floor below. Each chapter that bounds an argument by a limit states the rule with its own
identifier; this entry only defines the values.

| Field | Floor | Metal 4 (Apple7+) | D3D12 (FL 12_2) | Vulkan oracle |
|---|---|---|---|---|
| `maxViews` | 1,000,000 | 1,000,000 textures reachable per stage through argument buffers | at least 1,000,000 at resource binding tier 3; `OPTIONS19` may report more (shipping values *unverified*) | descriptor-heap size properties of `VK_EXT_descriptor_heap` |
| `maxSamplers` | 996 | 996 on Apple7 and Apple8, 500,000 on Apple9+, from `maxArgumentBufferSamplerCount` (*unverified*) | 2,048 shader-visible samplers; `OPTIONS19` may report more | sampler-heap size properties of `VK_EXT_descriptor_heap` |
| `maxRootBlockSize` | 65,536 | bounded by `maxBufferLength` | 65,536, the constant buffer limit kept as the portable cap | bounded by the buffer, read through a device address |
| `rootAlignment` | 256 | 4, raised to 256 | 256, `D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT` | `minUniformBufferOffsetAlignment`, at most 256 |
| `frameDataAlignment` | 256 | 256 | 256 | 256 |
| `maxBufferSize` | device | `maxBufferLength` | adapter memory; an `Addr` reaches only `maxAddrOffset` bytes of it | `maxBufferSize` (Vulkan 1.3) |
| `maxAddrOffset` | 536,870,911 (2^29 − 1) | `maxBufferLength` − 1, per device: an `Addr` is a GPU virtual address and reaches every byte of the largest buffer, and `createDevice` refuses a `maxBufferLength` below 2^32 | 2^29 − 1: an `Addr` is a raw-buffer heap index and a byte offset into that buffer's raw view, and a raw view holds at most 2^27 four-byte elements (`D3D12_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP`), 512 MiB | per device, up to 2^64 − 1: a buffer device address |
| `maxTextureDimension1D` | 16,384 | 16,384 | 16,384 | `maxImageDimension1D` |
| `maxTextureDimension2D` | 16,384 | 16,384 | 16,384 | `maxImageDimension2D` |
| `maxTextureDimension3D` | 2,048 | 2,048 | 2,048 | `maxImageDimension3D` |
| `maxTextureDimensionCube` | 16,384 | 16,384 | 16,384 | `maxImageDimensionCube` |
| `maxTextureLayers` | 2,048 | 2,048 | 2,048 | `maxImageArrayLayers` |
| `maxColorAttachments` | 8 | 8 | 8 | `maxColorAttachments` |
| `maxColorBytesPerSample` | 64 | 64, counted with Metal's per-format render-target sizes (*unverified*) | none below 8 targets of 16 bytes | none |
| `maxSamplerAnisotropy` | 16 | 16 | 16 | `maxSamplerAnisotropy` |
| `maxDrawIndirectCount` | 65,535 | 65,536, the largest count verified for the kernel-encoded indirect command buffer | `UINT` `MaxCommandCount` of `ExecuteIndirect` | `maxDrawIndirectCount`, at least 65,535 with `multiDrawIndirect` |
| `maxDispatchGroups` | 65,535 | bounded by 32-bit grid counts (*unverified*) | 65,535, `D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION` | `maxComputeWorkGroupCount` |
| `maxThreadsPerGroup` | 1,024 | `maxThreadsPerThreadgroup`, 1,024 | 1,024 | `maxComputeWorkGroupInvocations` |
| `maxGroupSize` | 1,024 x 1,024 x 64 | 1,024 per axis from `maxThreadsPerThreadgroup` | 1,024 x 1,024 x 64 | `maxComputeWorkGroupSize` |
| `maxGroupSharedMemory` | 32,768 | 32,768, `maxThreadgroupMemoryLength` | 32,768 | `maxComputeSharedMemorySize`, at least 16,384, per device |
| `maxMeshTaskGroups` | 65,535 | *unverified* | 65,535 per dimension of `DispatchMesh` | `maxMeshWorkGroupCount` or `maxTaskWorkGroupCount` |
| `maxMeshTaskGroupsTotal` | 4,194,304 | *unverified* | 2^22 total groups of `DispatchMesh` | `maxMeshWorkGroupTotalCount` |

Notes on the floor:

- `maxViews` counts slots a caller can hold. On D3D12 every buffer also takes one raw
  shader-resource slot, and one unordered-access slot for `ShaderWrite`, in the same heap (see
  [memory and buffers](03-memory-and-buffers.md)), so many small buffers reduce the views
  available; few large buffers with suballocation avoid that. The held slots 0 and 1 and the
  transient ring (`framesInFlight` × 1,024 slots) also take slots on D3D12.
- `maxSamplers` is the Metal 4 value on Apple7 and Apple8, below D3D12's 2,048; inline samplers in
  Metal shaders count toward it. A device reports its own, larger value where it has one.
- `rootAlignment` and `frameDataAlignment` are 256 everywhere because a D3D12 root constant
  buffer view requires it; Metal 4 needs only 4 bytes and Vulkan at most 256. See
  [shader ABI](05-shader-abi.md) and `CommandList::uploadFrameData`.
- `maxAddrOffset` bounds `Addr` arithmetic and dereference (`VU-rojo-buffer-0011` in
  [memory and buffers](03-memory-and-buffers.md#addr)). On the planned D3D12 backend an `Addr` is a
  raw-buffer heap index and a byte offset into that buffer's raw view, and a raw view holds at most
  2^27 four-byte elements, so one `Addr` base reaches 512 MiB and the portable value is 2^29 − 1
  even where `maxBufferSize` is larger. Shader arithmetic stays inside one view, so no encoding of a
  heap index and an offset reaches further. A larger buffer is valid: portable shader code reaches
  only its first 2^29 − 1 bytes through one `Addr`, and copies reach all of it. Metal 4 reports its
  own, larger value.
- `maxTextureLayers` counts cube faces: a cube array holds at most `maxTextureLayers / 6` cubes.
- `maxColorBytesPerSample` is a Metal tile-memory budget with no D3D12 counterpart; a render pass
  over it cannot run on Metal 4, so the floor binds every backend (`VU-rojo-cmd-0015` in
  [`RenderPassDesc`](07-command-recording.md#renderpassdesc)). The count is the sum of each color
  attachment format's bytes per texel, which is *unverified* against Metal's per-format
  render-target size table.
- `maxGroupSize` takes its z floor of 64 from D3D12; Metal 4 allows 1,024 on every axis. A
  threadgroup also stays within `maxThreadsPerGroup` in total.
- `maxDrawIndirectCount`'s floor mirrors the Vulkan oracle's minimum. Metal 4 reports the 65,536
  records [spike 3](../research/2026-10-12-spike-3-multi-draw-indirect.md) verified for its
  kernel-encoded indirect command buffer.

#### Valid usage
- None.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Source | `MTL::Device` properties (`maxBufferLength`, `maxArgumentBufferSamplerCount`, `maxThreadsPerThreadgroup`) and the Apple7+ feature set tables | `D3D12_REQ_*` constants, `D3D12_FEATURE_D3D12_OPTIONS19` heap sizes | `VkPhysicalDeviceLimits` and extension properties |
| Cost | cheap: mostly backend table constants | native | native |
