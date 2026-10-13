# 03. Memory and buffers

**Status**: Proposed (until spike 1 of ADR 0014 reports)
**API version**: 0.1.0

This chapter specifies memory classes, buffers, buffer addresses, per-frame data and residency;
placement heaps continue in [part 2](03-memory-and-buffers-part2.md). A buffer is memory with an
address: a shader reaches buffer memory only through an
[`Addr`](#addr) held in a root block or in other shader data, and dereferences it through the
[shader ABI](05-shader-abi.md). Handles, `Result` and label defaults are defined in
[fundamentals](01-fundamentals.md); destruction is deferred through
[`Device::destroy`](02-device.md#devicedestroy). Notation and cost classes follow
[conventions](00-conventions.md). Creation functions are free-threaded; a `CommandList` is recorded
by one thread at a time.

## Residency

Residency is invisible in the interface: every allocation a create function makes is resident for
its whole life, and no entry exposes residency control.

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Mechanism | one device-wide `MTL::ResidencySet`: `MTL::ResidencySet::addAllocation` on create, `MTL::ResidencySet::removeAllocation` on retirement, `MTL::ResidencySet::commit` on the next submit after a change; attached once with `MTL4::CommandQueue::addResidencySet` | committed and placed resources are resident implicitly; no `MakeResident` or `Evict` call is exposed | allocations are resident; no residency control is exposed |
| Cost | cheap: one set commit per submit that follows a change | native | native |

On Metal 4 every resource a shader can reach must sit in a committed residency set, including
memory reached only through an `Addr`; one large queue-attached set is the policy Apple advises and
Wicked Engine ships. On D3D12 the interface exposes no explicit residency control, so memory
pressure is the operating system's to resolve.

### `Memory`
```cpp
enum class Memory : uint8_t {
    Gpu,        // device-local; not CPU-visible
    Upload,     // CPU writes, GPU reads; persistently mapped
    Readback,   // GPU writes, CPU reads; persistently mapped
};
```
Memory classes are named by intent, not by backend heap type. `Upload` on Metal 4 uses the default
CPU cache mode, not write-combined: the consumer measured 42.6 ns per scattered small write through
write-combined memory. On D3D12 the `GPU_UPLOAD` heap type needs resizable BAR on discrete GPUs,
so its presence is reported by [`Device::capabilities`](02-device.md#devicecapabilities) and the
fallback is the classic upload heap.

The memory class fixes a buffer's copy roles. A `Gpu` buffer is a copy source and destination. An
`Upload` buffer is a copy source and shader-readable, never a copy destination, because a D3D12
`UPLOAD` heap resource stays in its fixed generic-read state. A `Readback` buffer is a copy
destination only. A fill destination is a `Gpu` buffer created with `BufferUsage::ShaderWrite`, as
[`CommandList::fillBuffer`](07-command-recording-part3.md#commandlistfillbuffer) requires.
[Copy commands](07-command-recording-part3.md) cite the rules below.
#### Valid usage
- `VU-rojo-buffer-0001` A buffer is the destination of a copy command only when its memory is
  `Gpu` or `Readback`.
- `VU-rojo-buffer-0002` A buffer is the source of a copy command only when its memory is `Gpu` or
  `Upload`.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| `Gpu` | `MTL::StorageModePrivate` | `D3D12_HEAP_TYPE_DEFAULT` | `VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT` |
| `Upload` | `MTL::StorageModeShared`, `MTL::CPUCacheModeDefaultCache` | `D3D12_HEAP_TYPE_GPU_UPLOAD` when reported, else `D3D12_HEAP_TYPE_UPLOAD` | `DEVICE_LOCAL \| HOST_VISIBLE \| HOST_COHERENT`, else `HOST_VISIBLE \| HOST_COHERENT` |
| `Readback` | `MTL::StorageModeShared` | `D3D12_HEAP_TYPE_READBACK` | `HOST_VISIBLE \| HOST_CACHED`, coherent type preferred (availability unverified) |
| Cost | native: unified memory | cheap: `GPU_UPLOAD` optional, `UPLOAD` fallback | cheap: device-local host-visible type when present, else host-visible |

### `BufferUsage`
```cpp
enum class BufferUsage : uint32_t {
    None         = 0,
    Index        = 1u << 0,   // index buffer of an indexed draw
    IndirectArgs = 1u << 1,   // arguments of an indirect draw or dispatch
    ShaderWrite  = 1u << 2,   // written by shaders through an Addr
    Sparse       = 1u << 3,   // reserved: no backing memory until tiles are mapped
};
constexpr BufferUsage operator|(BufferUsage, BufferUsage);
constexpr BufferUsage operator&(BufferUsage, BufferUsage);
```
Every `Gpu` and `Upload` buffer is shader-readable by address, and copy roles follow the memory
class ([`Memory`](#memory)), so `BufferUsage` carries only what a backend must know at creation.
Tiles of a `Sparse` buffer are mapped through [sparse resources](13-sparse.md). Sparse support is
reported by [`Device::capabilities`](02-device.md#devicecapabilities): on Metal 4 it is
`MTL::Device::supportsPlacementSparse()`, guaranteed on Apple8 and every Apple7 Mac but not on
every Apple7 GPU.
#### Valid usage
- `VU-rojo-buffer-0003` `ShaderWrite` and `Sparse` are set only on a `Memory::Gpu` buffer.
- `VU-rojo-buffer-0004` `Index` and `IndirectArgs` are not set on a `Memory::Readback` buffer.
- `VU-rojo-buffer-0005` `Sparse` is set only when `Device::capabilities` reports sparse resources.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| `Index`, `IndirectArgs` | no flag | no flag | `VK_BUFFER_USAGE_INDEX_BUFFER_BIT`, `VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT` |
| `ShaderWrite` | no flag | `D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS` and one raw UAV | no flag beyond `VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT` |
| `Sparse` | placement sparse buffer, `MTL::Device::newBuffer(length, options, placementSparsePageSize)` (signature unverified) | reserved resource | `VK_BUFFER_CREATE_SPARSE_BINDING_BIT \| VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT` |
| Cost | native | native | native |

### `BufferDesc`
```cpp
struct BufferDesc {
    uint64_t size = 0;                        // bytes
    Memory memory = Memory::Gpu;              // memory class
    BufferUsage usage = BufferUsage::None;    // Index | IndirectArgs | ShaderWrite | Sparse
    std::string_view label;                   // empty: a rojorhi.-prefixed default
};
```
On D3D12 each buffer costs global heap slots for its raw descriptors (see
[`Device::createBuffer`](#devicecreatebuffer)), so the reference recommends few large buffers with
suballocation through [`Addr`](#addr) offsets over many small ones.
#### Valid usage
- `VU-rojo-buffer-0006` `size` is greater than zero and at most the maximum buffer size in
  [limits](15-formats-and-limits-part2.md#limits).
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Descriptor | `length` and `MTL::ResourceOptions` | `D3D12_RESOURCE_DESC1` with `D3D12_RESOURCE_DIMENSION_BUFFER` | `VkBufferCreateInfo` |
| Cost | native | native | native |

### `Device::createBuffer`
```cpp
Result<Buffer> Device::createBuffer(const BufferDesc& desc,
                                    std::span<const std::byte> initial = {},
                                    Placement placement = {});
```
Creates a buffer of `desc.size` bytes in `desc.memory`, labels it and adds it to the residency
set. `Upload` and `Readback` buffers are persistently mapped. Non-empty `initial` fills the first
`initial.size()` bytes; the rest is undefined. For a `Gpu` buffer the backend stages `initial` and
copies it, and the copy completes before any work submitted after the call returns; for `Upload`
the bytes are copied through the CPU pointer. A non-null `placement.heap` creates the buffer inside
that heap (see [`Placement`](03-memory-and-buffers-part2.md#placement)). Out of memory and a native
refusal are error results.
#### Valid usage
- `VU-rojo-buffer-0007` `initial.size()` is at most `desc.size`.
- `VU-rojo-buffer-0008` `initial` is empty when `desc.usage` contains `Sparse` or `desc.memory` is
  `Readback`.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Allocation | `MTL::Device::newBuffer(length, options)`; placed: `MTL::Heap::newBuffer(length, options, offset)` | `ID3D12Device10::CreateCommittedResource3`; placed: `CreatePlacedResource2`; sparse: `CreateReservedResource2` | `vkCreateBuffer`, `vkAllocateMemory`, `vkBindBufferMemory` |
| Descriptors | none: shaders use `MTL::Buffer::gpuAddress()` | one raw SRV (`CreateShaderResourceView`, `DXGI_FORMAT_R32_TYPELESS`, `D3D12_BUFFER_SRV_FLAG_RAW`) and, for `ShaderWrite`, one raw UAV (`CreateUnorderedAccessView`, `D3D12_BUFFER_UAV_FLAG_RAW`) in the global heap; none for `Readback` | none: shaders use `vkGetBufferDeviceAddress` |
| Initial data | staged `MTL4::ComputeCommandEncoder::copyFromBuffer`, or `memcpy` into `MTL::Buffer::contents()` | staged `CopyBufferRegion`, or `memcpy` into the mapped pointer | staged `vkCmdCopyBuffer2`, or `memcpy` into the mapped pointer |
| Cost | native | cheap: one or two descriptor writes and heap slots per buffer | native |

### `Addr`
```cpp
struct alignas(8) Addr { uint32_t lo, hi; };   // backend-defined encoding
Addr operator+(Addr base, uint64_t offset);     // offset bytes further into the same buffer
```
`Addr` is an 8-byte value naming buffer memory; its encoding is backend-defined, and the
value-initialized `Addr{}` (both words zero) is the null address on every backend. It supports
offset arithmetic, so a consumer may suballocate inside one large buffer. In shaders `Addr` is
`typealias Addr = uint2;`, never a struct, and every `Addr` field in a root block sits at an
8-byte-aligned offset (see [shader ABI](05-shader-abi.md)). Because the D3D12 form is not a virtual
address, structs shared with shaders carry `Addr`, never a raw 64-bit pointer, and only the shader
module dereferences it. Whether `Addr` chasing is cheap enough on D3D12 to recommend for hot paths
is spike 1; until it reports, the reference recommends root fields and heap indices for per-draw
hot data.

A shader never dereferences the null address, and it writes through an address only into a buffer
created with `BufferUsage::ShaderWrite`. No tier diagnoses either, because shader accesses are
invisible to the CPU; the shader-side requirements are stated with
[`rojoRHI::load` and `rojoRHI::store`](05-shader-abi-part2.md#addresses). Likewise, a shader that
chases an address, including one stored in buffer memory, finishes before the buffer it names is
destroyed; the submission of that work precedes the destruction, and no tier diagnoses the
shader-side access.
#### Valid usage
- `VU-rojo-buffer-0009` The byte position named by `base + offset` lies inside the buffer `base`
  names.
- `VU-rojo-buffer-0010` Retired in 0.1.0: Work that dereferences an address is submitted before
  the buffer it names is destroyed.
- `VU-rojo-buffer-0011` Every address `operator+` produces, and every byte reached through an
  address, lies less than
  [`Limits::maxAddrOffset`](15-formats-and-limits-part2.md#limits) bytes from the start of the
  buffer it names; the portable value is 2^32 − 1, *unverified* until the D3D12 raw-view element
  limit is settled ([limits](15-formats-and-limits-part2.md#limits)).
- `VU-rojo-buffer-0019` Work recorded with an address passed to a recorded command is submitted
  before the buffer the address names is destroyed. Checked by the validation tracker.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Representation | GPU virtual address | raw-buffer heap index in `lo` (a load uses `index`, a store `index + 1`) and 32-bit byte offset in `hi`; descriptor slot 0 is reserved so the zero `Addr` is null | buffer device address |
| Dereference | `device T*` load | `ByteAddressBuffer` from `ResourceDescriptorHeap[index]`, then `Load<T>` at the offset | `PhysicalStorageBuffer` load |
| Cost | native | costly: one descriptor fetch per base, `NonUniformResourceIndex` when divergent; spike 1 | native |

### `Device::address`
```cpp
Addr Device::address(Buffer buffer, uint64_t offset = 0);
```
Returns the shader-visible address of byte `offset` of `buffer`. The call is free-threaded, issues
no native call on any backend and returns the same value every time for the buffer's lifetime.
#### Valid usage
- `VU-rojo-buffer-0012` `offset` is less than the size of `buffer`.
- `VU-rojo-buffer-0013` `buffer` was not created with `Memory::Readback`.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::Buffer::gpuAddress()` plus `offset` | the buffer's raw-view heap index paired with `offset` | `vkGetBufferDeviceAddress`, cached at creation, plus `offset` |
| Cost | native | cheap: composes an index and an offset | native |

### `Device::cpuPointer`
```cpp
std::byte* Device::cpuPointer(Buffer buffer);
```
Returns the CPU pointer to byte 0 of a persistently mapped buffer; it stays valid until the buffer
is destroyed. The call is free-threaded, and threads may access distinct bytes through the pointer
concurrently. RojoRHI performs no synchronization: the caller orders CPU access against GPU work
with fences or frame pacing (see [queues and frames](10-queues-and-frames.md)).

The CPU does not write bytes of an `Upload` buffer that submitted, unretired work reads, and it
reads bytes of a `Readback` buffer only after a fence value signalled after the writing work has
completed. Both are data races between the CPU and the GPU that no tier diagnoses, because CPU
accesses through the pointer are invisible to RojoRHI.
#### Valid usage
- `VU-rojo-buffer-0014` `buffer` was created with `Memory::Upload` or `Memory::Readback`.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::Buffer::contents()` | `ID3D12Resource::Map`, once at creation | `vkMapMemory`, once at creation |
| Cost | native | native | native |

### `CommandList::uploadFrameData`
```cpp
Addr CommandList::uploadFrameData(std::span<const std::byte> bytes, uint32_t alignment = 256);
```
Performs one suballocation of `bytes.size()` bytes at `alignment` from the open frame's arena and
one copy, and returns the address. The arena grows when exhausted, and each growth is counted (see
[diagnostics](14-diagnostics.md)); failing to grow is fatal. The address is valid only within the
frame that produced it, and its memory is recycled when
[`Device::beginFrame`](02-device.md#devicebeginframe) reuses the frame slot. Binding a root block
is a separate call ([`CommandList::setRoot`](05-shader-abi.md)), so a root may point at persistent
data that is never re-uploaded; the default alignment satisfies the root-block placement rule.

Frame data may serve as a root block, as data a shader reads through an address, and as the
arguments and count of an indirect command ([GPU-driven rendering](08-gpu-driven.md)): the arena's
memory counts as a buffer created with `BufferUsage::IndirectArgs`. The Debug tier checks
`VU-rojo-buffer-0020` only where a command takes the address. It does not see an address reached
through a root block or other buffer memory, or a shader's read, and it cannot tell a stale address
once a list of a later frame holds the same arena memory again. An address carries no generation,
so one into memory reused by a newer buffer names that buffer.
#### Valid usage
- `VU-rojo-buffer-0015` `bytes` is not empty.
- `VU-rojo-buffer-0016` `alignment` is a power of two.
- `VU-rojo-buffer-0017` A frame is open: `Device::beginFrame` has been called and the matching
  `Device::endFrame` has not.
- `VU-rojo-buffer-0018` Retired in 0.1.0: Only work submitted before the `Device::endFrame` that
  closes the producing frame reads the returned address. The Debug tier sees only the commands that
  take the address, not the shader reads through memory, so it checks the recording-time form,
  `VU-rojo-buffer-0020`.
- `VU-rojo-buffer-0020` The address is passed to a command only on a list acquired in the frame that
  produced it.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Arena | per-frame-slot `MTL::StorageModeShared` pages; `MTL::Buffer::gpuAddress()` plus offset | per-frame-slot buffers in the `Upload` heap type; page raw-view index and offset | per-frame-slot host-visible buffers; device address plus offset |
| Growth | a new page from `MTL::Device::newBuffer`, added to the residency set | a new committed buffer and raw SRV | a new buffer and allocation |
| Cost | cheap: one bump allocation and one copy | cheap: one bump allocation and one copy | cheap: one bump allocation and one copy |
