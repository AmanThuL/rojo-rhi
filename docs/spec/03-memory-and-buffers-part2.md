# 03. Memory and buffers (part 2)

**Status**: Accepted
**API version**: 0.1.0

This part specifies placement heaps; memory classes, buffers, addresses, per-frame data and
residency are in [part 1](03-memory-and-buffers.md). Placement serves transient aliasing, whose
policy belongs to the consumer's graph, and backs [sparse resources](13-sparse.md). The first use of
aliased memory is a barrier with an undefined old layout (see
[synchronization](09-synchronization.md)). Every function here is free-threaded.

### `HeapDesc`
```cpp
struct HeapDesc {
    uint64_t size = 0;             // bytes
    Memory memory = Memory::Gpu;   // memory class of every resource placed inside
    std::string_view label;        // empty: a rojorhi.-prefixed default
};
```
A heap is one device allocation that buffers and textures are placed inside at explicit offsets.
It tracks no hazards: placed resources whose byte ranges overlap alias, and the caller orders them.
A backend may round `size` up to its allocation granularity; the usable size is never less than
`size`.
#### Valid usage
- `VU-rojo-heap-0001` `size` is greater than zero.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Descriptor | `MTL::HeapDescriptor` with `MTL::HeapDescriptor::setType(MTL::HeapTypePlacement)`, `setSize`, `setStorageMode`, `setCpuCacheMode` | `D3D12_HEAP_DESC` with the heap type of `memory` and `D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES` (resource heap tier 2, unverified against the floor) | `VkMemoryAllocateInfo` with the memory type of `memory` |
| Cost | native | native | native |

### `Device::createHeap`
```cpp
Result<Heap> Device::createHeap(const HeapDesc& desc);
```
Creates a placement heap, labels it and adds it to the residency set. Out of memory is an error
result.
#### Valid usage
- None.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::Device::newHeap(descriptor)`, `MTL::ResidencySet::addAllocation` | `ID3D12Device::CreateHeap` | `vkAllocateMemory` |
| Cost | native | native | native |

### `Device::placementRequirements`
```cpp
struct PlacementRequirements {
    uint64_t size = 0;        // bytes the resource occupies in a heap
    uint64_t alignment = 0;   // power of two the heap offset is a multiple of
};
PlacementRequirements Device::placementRequirements(const BufferDesc& desc);
PlacementRequirements Device::placementRequirements(const TextureDesc& desc);
```
Returns the bytes a buffer or texture described by `desc` occupies inside a heap and the alignment
its offset must satisfy. The answer is the backend's for that exact descriptor, so a caller packing
a heap asks rather than computing sizes from texel counts. The call is free-threaded, allocates
nothing and returns the same value for equal descriptors on one device.
#### Valid usage
- `VU-rojo-heap-0002` `desc` satisfies the rules of its descriptor
  ([`BufferDesc`](03-memory-and-buffers.md#bufferdesc) or
  [`TextureDesc`](04-textures-and-heaps.md#texturedesc)), and its usage does not contain `Sparse`.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::Device::heapBufferSizeAndAlign`, `MTL::Device::heapTextureSizeAndAlign` | `ID3D12Device12::GetResourceAllocationInfo3`, with the castable formats the texture is created with | `vkGetDeviceBufferMemoryRequirements`, `vkGetDeviceImageMemoryRequirements` |
| Cost | native | native | native |

### `Placement`
```cpp
struct Placement {
    Heap heap;             // null: a dedicated allocation
    uint64_t offset = 0;   // byte offset inside heap
};
```
Passed to [`Device::createBuffer`](03-memory-and-buffers.md#devicecreatebuffer) and
[`Device::createTexture`](04-textures-and-heaps.md#devicecreatetexture). For the rules below,
`req` is [`Device::placementRequirements`](#deviceplacementrequirements) of the descriptor being
created. A heap outlives every resource placed in it, as `VU-rojo-device-0008` of
[`Device::destroy`](02-device.md#devicedestroy) requires.
#### Valid usage
- `VU-rojo-heap-0003` When `heap` is not null, `offset` plus `req.size` is at most the heap's size.
- `VU-rojo-heap-0004` When `heap` is not null, `offset` is a multiple of `req.alignment`.
- `VU-rojo-heap-0005` When `heap` is not null, the resource's memory class equals the heap's; a
  texture is placed only in a `Memory::Gpu` heap.
- `VU-rojo-heap-0006` When `heap` is not null, the resource is not created with `Sparse` usage.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::Heap::newBuffer(length, options, offset)`, `MTL::Heap::newTexture(descriptor, offset)` | `ID3D12Device10::CreatePlacedResource2` | `vkBindBufferMemory`, `vkBindImageMemory` at `offset` |
| Cost | native | native | native |
