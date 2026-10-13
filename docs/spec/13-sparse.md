# 13. Sparse resources

**Status**: Accepted
**API version**: 0.1.0

A sparse resource reserves address space without backing memory. `BufferUsage::Sparse`
([memory and buffers](03-memory-and-buffers.md)) and `TextureUsage::Sparse`
([textures and heaps](04-textures-and-heaps.md)) create one; its tiles are then mapped onto, and
unmapped from, ranges of a `Heap` ([memory and buffers](03-memory-and-buffers.md)) by a queue
operation, when `Capabilities::placementSparse` ([device](02-device.md)) reports support. Sparse
resources are native on all three APIs: placement sparse resources on Metal 4, reserved resources on
D3D12, and sparse buffers and images on Vulkan. On Metal 4 they need
`MTL::Device::supportsPlacementSparse`, which every Apple7 Mac and every Apple8 or newer GPU
reports.

Tiles are 64 KiB on Metal 4 and D3D12. The Metal 4 backend creates sparse resources with a 64 KiB
sparse page size (`MTL::SparsePageSize64`) and creates every `Memory::Gpu` heap able to back pages
of that size, matching D3D12's fixed 64 KiB tile. On Vulkan the tile size is the resource's
`VkMemoryRequirements::alignment`, which the implementation chooses; `TileShape::bytes` reports it.

Tile shapes come from the backend per format and sample count. Accessing an unmapped tile does not
fault: reads return undefined values and writes are discarded. A tile newly mapped onto heap memory
holds that memory's contents, so its first use follows the aliasing rule of
[synchronization](09-synchronization.md): a barrier from `Layout::Undefined` for a texture, a global
barrier for a buffer.

### `TileShape`
```cpp
struct TileShape {
    uint32_t width = 0, height = 0, depth = 0;
    uint32_t bytes = 0;
    uint32_t mipTailFirst = 0;
    uint64_t mipTailBytes = 0;
};
```

| Field | Meaning |
|---|---|
| `width`, `height`, `depth` | Texels one tile covers in each axis; for a buffer, `width` is the tile size in bytes and `height` and `depth` are 1 |
| `bytes` | Heap bytes one tile maps; every heap offset and buffer range in a mapping is a multiple of it |
| `mipTailFirst` | First mip level packed into the mip tail; equals the texture's mip count when it has no tail; zero for a buffer |
| `mipTailBytes` | Heap bytes the mip tail of one layer needs; zero when there is no tail |

Mip levels below `mipTailFirst` are mapped tile by tile; the levels from `mipTailFirst` up are
packed into one tail per layer and mapped as a unit.

#### Valid usage
- None.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Tile extent | `MTL::Device::sparseTileSize` with the texture type, pixel format, sample count and `MTL::SparsePageSize64` | `D3D12_TILE_SHAPE` | `VkSparseImageFormatProperties::imageGranularity` |
| Tile bytes | 64 KiB, `MTL::SparsePageSize64` | 64 KiB, `D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES` | `VkMemoryRequirements::alignment` |
| Mip tail | `MTL::Texture::firstMipmapInTail`, `MTL::Texture::tailSizeInBytes` | `D3D12_PACKED_MIP_INFO` | `imageMipTailFirstLod`, `imageMipTailSize`; a format with `VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT` has one tail shared by all layers, mapped once through layer 0 |
| Cost | native | native | native |

### `Device::tileShape`
```cpp
TileShape Device::tileShape(Texture texture);
TileShape Device::tileShape(Buffer buffer);
```
Returns the tile shape of a sparse texture or buffer. The shape is fixed at creation, so the call
records no GPU work and is free-threaded.

#### Valid usage
- `VU-rojo-sparse-0001` The texture or buffer was created with `TextureUsage::Sparse` or
  `BufferUsage::Sparse`.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::Device::sparseTileSize` and the texture's tail properties, cached at creation | `ID3D12Device::GetResourceTiling`, cached at creation | `vkGetImageSparseMemoryRequirements2` or `vkGetBufferMemoryRequirements2`, cached at creation |
| Cost | native | native | native |

### `TileMapping`
```cpp
struct TileMapping {
    Texture texture = {}; TextureRegion region; bool mipTail = false;
    Buffer buffer = {}; uint64_t bufferOffset = 0, bufferSize = 0;
    Heap heap = {}; uint64_t heapOffset = 0; bool unmap = false;
};
```
One change to the mapping of a texture region, a texture's mip tail, or a buffer byte range.

| Field | Meaning |
|---|---|
| `texture` | Sparse texture to map, or null when `buffer` is set |
| `region` | Texels of one mip level, over a range of layers, to map ([`TextureRegion`](07-command-recording-part3.md)); with `mipTail`, only its layer range is read |
| `mipTail` | Maps the mip tail of each of `region`'s layers instead of a region |
| `buffer` | Sparse buffer to map, or null when `texture` is set |
| `bufferOffset`, `bufferSize` | Byte range of `buffer` to map |
| `heap` | Heap supplying the memory; null with `unmap` |
| `heapOffset` | Byte offset of the first tile in `heap` |
| `unmap` | Removes the mapping of the named tiles instead of adding one |

The tiles of a region map onto consecutive heap tiles from `heapOffset`, in order of x, then y,
then z, then layer; mip tails map one after another, one per layer, each `mipTailBytes` long. A
buffer range maps onto consecutive heap bytes from `heapOffset`.

#### Valid usage
- `VU-rojo-sparse-0002` Exactly one of `texture` and `buffer` is not null.
- `VU-rojo-sparse-0003` The texture or buffer was created with `TextureUsage::Sparse` or
  `BufferUsage::Sparse`.
- `VU-rojo-sparse-0004` Without `mipTail`, `region` lies in one mip level below `mipTailFirst`, has
  a nonzero extent and a layer range inside the texture, its origin is a multiple of the tile
  shape, and its extent is a multiple of the tile shape or reaches the edge of the level.
- `VU-rojo-sparse-0005` With `mipTail`, the texture has a mip tail and `region`'s layer range is
  not empty and lies inside the texture.
- `VU-rojo-sparse-0006` `mipTail` is false when `buffer` is set.
- `VU-rojo-sparse-0007` For a buffer, `bufferOffset` is a multiple of `bytes`, `bufferSize` is
  greater than zero and a multiple of `bytes` or reaches the end of the buffer, and the range lies
  inside the buffer.
- `VU-rojo-sparse-0008` Without `unmap`, `heap` is not null, `heapOffset` is a multiple of `bytes`,
  and every mapped tile lies inside the heap.
- `VU-rojo-sparse-0009` Without `unmap`, `heap` was created with `Memory::Gpu`, the only memory
  class that backs sparse tiles.
- `VU-rojo-sparse-0010` With `unmap`, `heap` is null and `heapOffset` is zero.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Texture region | `MTL4::UpdateSparseTextureMappingOperation` with `textureRegion`, `textureLevel`, `textureSlice`, `heapOffset` | `D3D12_TILED_RESOURCE_COORDINATE` and `D3D12_TILE_REGION_SIZE` | `VkSparseImageMemoryBind` |
| Mip tail | the same operation naming the first tail level | a coordinate in the first packed mip | `VkSparseMemoryBind` at `imageMipTailOffset` |
| Buffer range | `MTL4::UpdateSparseBufferMappingOperation` | buffer tile coordinates | `VkSparseBufferMemoryBindInfo` |
| Unmap | `MTL::SparseTextureMappingModeUnmap` | `D3D12_TILE_RANGE_FLAG_NULL` | `VK_NULL_HANDLE` memory |
| Cost | native | native | native |

### `Queue::mapTiles`
```cpp
void Queue::mapTiles(std::span<const TileMapping> mappings);
```
Applies `mappings` in order, as a queue operation: after all work submitted earlier to this queue
and before any work submitted to it later. Work on other queues is ordered against the mapping by
fences, by submitting a batch that waits on or signals a `FenceValue`
([queues and frames](10-queues-and-frames.md)) around the call. The call does not wait, and is
serialized with submission on the queue. Any queue type may map tiles. A heap stays alive while
any tile remains mapped from it (`VU-rojo-device-0009`, [device](02-device.md)). Destroying a
sparse texture or buffer unmaps all its tiles at the call, as if each had been unmapped, so a heap
that only it kept alive may then be destroyed.

#### Valid usage
- `VU-rojo-sparse-0011` Retired in 0.1.0: Every element of `mappings` satisfies the `TileMapping`
  rules.
- `VU-rojo-sparse-0012` No two elements of `mappings` name the same tile.
- `VU-rojo-sparse-0013` Work on another queue that accesses a tile whose mapping changes is
  complete, or ordered against the mapping by fences, before the mapping executes. Checked by the
  validation tracker.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::CommandQueue::updateTextureMappings` and `MTL4::CommandQueue::updateBufferMappings` | `ID3D12CommandQueue::UpdateTileMappings` | `vkQueueBindSparse` |
| Ordering | queue order | queue order | timeline semaphores against the queue's submissions; the queue's family needs `VK_QUEUE_SPARSE_BINDING_BIT` |
| Cost | native | native | native |
