# 05. Shader ABI

**Status**: Proposed (until spike 1 of ADR 0014 reports)
**API version**: 0.1.0

This chapter defines how a shader reaches data: the root block, the `RojoRHI.slang` module and the
layout rules shared by C++ and Slang. It continues in [part 2](05-shader-abi-part2.md), which
defines the `Addr` accessors, heap-index fetches and built-ins. Notation, cost classes and
valid-usage identifiers follow [conventions](00-conventions.md). The decision behind this chapter
is [ADR 0011](../decisions/0011-shader-abi.md).

## Model

A shader reaches all data through three paths: one root block per draw or dispatch, bound by
address; `Addr` values inside it (see [memory and buffers](03-memory-and-buffers.md)); and 32-bit
heap indices, `ViewIndex` and `SamplerIndex`, stored as `uint` fields (see
[textures and heaps](04-textures-and-heaps.md)). There are no binding slots, no descriptor sets,
no pipeline layouts and no per-pipeline binding declarations; every backend uses one fixed global
layout. There is no vertex-input state either: vertices are pulled by the shader through
`rojoRHI::load`, while index buffers stay a per-draw argument (see
[command recording](07-command-recording.md)) to keep hardware index fetch and the post-transform
cache.

RojoRHI ships the Slang module `Shaders/RojoRHI/RojoRHI.slang`, which every shader imports with
`import RojoRHI;`; its symbols live in namespace `rojoRHI`, the namespace
[ADR 0001](../decisions/0001-identity-and-naming.md) fixes, and the module and directory names
follow the file and directory conventions of [naming](../conventions/naming.md). It is the only
place that knows how each target lowers the ABI. Only its functions dereference an `Addr`:
shaders never use raw pointer syntax, because Slang emits invalid HLSL for pointer types without a
diagnostic.

`Addr` is an 8-byte value naming buffer memory. In Slang it is a type alias, never a struct; in C++
it is an 8-byte-aligned struct of two 32-bit words:

```slang
typealias Addr = uint2;                          // RojoRHI.slang
```

```cpp
struct alignas(8) Addr { uint32_t lo, hi; };    // rojoRHI public header
```

A struct member in an HLSL `cbuffer` starts on a 16-byte boundary, so a struct `Addr` would put
`DrawRoot::instances` below at offset 16 on HLSL and 8 on Metal. An 8-aligned `uint2` never
straddles a 16-byte row, so every target agrees.

```slang
import RojoRHI;

struct DrawRoot { Addr vertices; Addr instances; uint albedo; uint linearClamp; uint drawId; uint pad; };

[shader("vertex")]
VsOut vsMain(uint vertexId : SV_VertexID)
{
    DrawRoot root = rojoRHI::root<DrawRoot>();
    Vertex v = rojoRHI::load<Vertex>(root.vertices, rojoRHI::vertexIndex(vertexId));
    // ...
}

[shader("fragment")]
float4 psMain(VsOut input) : SV_Target
{
    DrawRoot root = rojoRHI::root<DrawRoot>();
    return rojoRHI::texture2D(root.albedo).Sample(rojoRHI::sampler(root.linearClamp), input.uv);
}
```

A bindless shader's reads are invisible to the CPU, so no tier can diagnose how a shader calls the
`rojoRHI` functions, and their entries carry no valid-usage rules. Each states its requirements as
normative prose instead; meeting them is the shader author's obligation, and a violation gives
undefined results on the GPU.

## Layout rules

A struct shared between C++ and Slang follows one of two layouts.

The root block is read as constant data, whose offsets differ between Metal, HLSL `cbuffer` and
std140 SPIR-V once nesting or three-component vectors appear. A root type is therefore flat:

- Members are 32-bit scalars, two- or four-component vectors of them, and `Addr`.
- No nested structs: a nested struct starts on a 16-byte boundary in HLSL and std140 but not on
  Metal, which moves every later member.
- No three-component vectors: `uint3` and `float3` take 16 bytes in a Metal constant buffer and 12
  elsewhere.
- Every member starts at a multiple of its own alignment: 4 bytes for scalars, 16 for
  four-component vectors, and 8 for two-component vectors and every `Addr` field. Padding is
  explicit, as in `DrawRoot::pad` above.
- The struct is 16-byte aligned and its size is a multiple of 16 bytes.
- The block is placed at 256-byte alignment, the D3D12 root constant-buffer requirement, and is at
  most 65536 bytes, the D3D12 constant-buffer limit, kept as the portable cap.

Data behind an `Addr` uses natural layout on every target: C++ `offsetof` and `sizeof`, with
nested structs allowed and three-component vectors packed to 12 bytes (`packed_float3` on Metal).
A shared struct names buffer memory with `Addr`, never with a raw 64-bit pointer, because the D3D12
form of an address is not a virtual address. No shared struct contains a texture, sampler or handle
type, a pointer to one, or an unsized array.

## Layout checking

Every struct shared between C++ and Slang is checked at build time: a tool compares the Slang
reflection offsets on each compiled target with the C++ `offsetof` values and fails the build on
any drift. The rules above keep the check passing; the check, not the rules, is the authority.

## Shader build, ABI version and Slang upgrades

The xmake shader rule compiles Slang offline to a metallib for Metal 4, and is to compile DXIL once
the planned D3D12 backend exists. It compiles SPIR-V in CI only, so the Vulkan mapping is checked
mechanically rather than on paper. RojoRHI never compiles shaders at run time. The rule stamps
`ROJORHI_SHADER_ABI_VERSION` into every library it builds, and `Device::loadShaderLibrary` (see
[pipelines](06-pipelines.md)) checks the stamp.

Slang's Metal bindless support is young and fails silently in places: handle pointers inside
structs, unsized arrays and combined texture-samplers miscompile, and HLSL pointer text is emitted
without an error. A compile-and-run test of the exact ABI in this chapter, including the lowering
of every built-in, runs on every compiled target whenever the pinned Slang version changes, and an
upgrade lands only when it passes. Background:
[Metal 4 and Slang state](../research/2026-10-11-rhi-redesign/06-metal4-slang-state.md).

## Entries

### `CommandList::setRoot`

```cpp
void CommandList::setRoot(Addr root);
```

Binds `root` as the root block of every later draw and dispatch recorded on this command list,
until the next `setRoot` or `pushRoot`. A command list starts with no root; the binding persists
across render passes and encoder boundaries, and every stage of a draw reads the same root. Only
the address is captured at recording; the bytes are read when each draw or dispatch executes, so
GPU work may write a root ahead of its use, ordered by a barrier (see
[synchronization](09-synchronization.md)). A static root built once in persistent memory is bound
without re-uploading it.

#### Valid usage

- `VU-rojo-abi-0001` `root` is not a null address.
- `VU-rojo-abi-0002` The byte offset of `root` within its buffer is a multiple of 256.
- `VU-rojo-abi-0003` `root` names a buffer that stays alive until every draw and dispatch reading it
  completes, or frame data produced in the frame being recorded. Checked by the validation tracker.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::ArgumentTable::setAddress(root, 0)` on the table bound to every stage | `SetGraphicsRootConstantBufferView` or `SetComputeRootConstantBufferView` on `b0` of the one global root signature, with the GPU virtual address looked up from the `Addr`'s heap index plus its offset | `vkCmdPushDataEXT` of the address, or a push-constant `Addr` |
| Cost | native: one table write | cheap: index→GPU VA lookup | native |
| Constraint | none | 256-byte alignment of the root address; 65536-byte cap | none: the root is read through the pushed address |

### `CommandList::pushRoot`

```cpp
template <class T> Addr CommandList::pushRoot(const T& value);
```

Copies `value` into the open frame's arena at 256-byte alignment, binds the copy exactly as
`setRoot` does, and returns its address. It is equivalent to
`Addr a = uploadFrameData(std::as_bytes(std::span(&value, 1)), 256); setRoot(a); return a;`: one
suballocation, one copy and one bind, with the valid usage of `CommandList::uploadFrameData` (see
[memory and buffers](03-memory-and-buffers.md)). The returned address is valid only within the
current frame and may be bound again with `setRoot` without another copy.

Requirements: `T` is trivially copyable and standard-layout, follows the root layout rules of this
chapter, and is at most 65536 bytes. They hold at compile time: `pushRoot` rejects a type that is
not trivially copyable and standard-layout, or that is too large, with `static_assert`, and the
build-time [layout check](#layout-checking) rejects a root type that breaks the layout rules.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | frame-arena copy, then `setAddress(copy, 0)` | frame-arena copy, then the root CBV write | frame-arena copy, then `vkCmdPushDataEXT` of its address |
| Cost | cheap: one suballocation and copy; Metal 4 has no inline root bytes | cheap: one suballocation and copy; root constants are not used, since the call returns an address | cheap: one suballocation and copy; inline push data is not used, for the same reason |

### `rojoRHI::root`

```slang
T root<T>();
```

Returns the root block bound for the executing draw or dispatch, read as `T` under the target's
constant-buffer layout. Every stage of a draw that calls it reads the same bytes.

Requirements: `T` follows the root layout rules of this chapter and is at most 65536 bytes; a root
naming at least `sizeof(T)` bytes was bound on the command list before the draw or dispatch that
executes the call; and no GPU work writes the root's bytes while a draw or dispatch reading them
executes.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Lowering | `constant T&` at argument-table `buffer(0)` | `cbuffer` at `b0`, a root CBV | a buffer reference at the pushed address |
| Cost | native | native | native |
| Constraint | none | 256-byte alignment of the root address; 65536-byte cap | none: a buffer reference has no uniform-range limit |

### `ROJORHI_SHADER_ABI_VERSION`

```slang
static const uint ROJORHI_SHADER_ABI_VERSION = 1;   // RojoRHI.slang
```

A positive integer naming the ABI contract this chapter defines: the root binding, the `Addr`
encoding, the hidden heap tables and the lowering of every built-in. It increases whenever any of
them changes. `RojoRHI.slang` declares it as a constant rather than a macro, because macros do not
cross a Slang `import`. The shader rule reads the value from that file and stamps it into every
compiled library, and `Device::loadShaderLibrary` returns an error for a library whose stamp is
missing or differs from the value RojoRHI itself was built with, instead of running a stale shader.
The value is independent of the `ROJORHI_VERSION_*` macros (see [fundamentals](01-fundamentals.md)).

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Stamp | carried in the metallib by the shader rule; mechanism unverified | carried in the DXIL container by the shader rule; mechanism unverified | none: SPIR-V is compiled in CI only |
| Check | one integer compare in `loadShaderLibrary` | one integer compare in `loadShaderLibrary` | the CI compile |
| Cost | native | native | native |
