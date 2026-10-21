# 09. Synchronization

**Status**: Accepted
**API version**: 0.1.0

Barriers are explicit and batched. Nothing is tracked in Release builds: the caller, usually a
render graph, states every dependency, and RojoRHI lowers it without inspecting resource state.

The validation tracker ([diagnostics](14-diagnostics.md)) exists only in non-Release (Debug)
builds and runs when [`DeviceDesc::validation`](02-device.md) is enabled. It records every barrier
and the accesses it can observe: attachments, copy and clear endpoints, index reads,
indirect-argument reads, upscaler inputs and outputs, presented images, and shader accesses
declared with [`CommandList::declareAccess`](14-diagnostics.md). Accesses a shader
makes through heap indices or `Addr` values without a declaration are invisible to the CPU and are
not tracked. For the accesses it observes, the tracker reports a missing barrier, a stage or access
that does not cover the actual use, a wrong old layout, and a texture used in a layout that does
not permit the use. Checking necessity as well as sufficiency means a GPU that happens to order two
accesses without a barrier no longer hides the missing one.

A barrier is a dependency from a source scope to a destination scope. Each side names a `Stage`
set and an `Access` set; a texture barrier also names the layout before and after. The barrier
orders every command earlier in queue submission order on the same queue, within the scope of its
source side, before every later command within the scope of its destination side, and makes the
source accesses visible to the destination accesses. Submission order runs through the lists of
one [`Queue::submit`](10-queues-and-frames.md) in span order and then across submits. Barriers never
order work on two queues; fences do ([queues and frames](10-queues-and-frames.md)).

This chapter defines the stage, access and layout vocabulary. The barrier descriptors, the batched
call, cross-queue handoff and aliasing are in [part 2](09-synchronization-part2.md).

## Accesses commands make

Each command performs the uses below; a barrier between two uses names the use's stage and access
on the matching side.

| Use | Stage | Access | Texture layouts that permit it |
|---|---|---|---|
| Indirect arguments and count ([GPU-driven](08-gpu-driven.md)) | `IndirectArgs` | `IndirectArgs` | buffers only |
| Index buffer of an indexed draw | `IndexInput` | `IndexRead` | buffers only |
| Shader read through an `Addr` or a `Sampled` view | the shader's stage | `ShaderRead` | `ShaderRead`, `DepthStencilRead`, `General` |
| Shader read or write through a `Storage` view, or a write through an `Addr` | the shader's stage | `ShaderRead`, `ShaderWrite` | `ShaderWrite` |
| Color attachment load, blend and store | `ColorAttachment` | `ColorAttachmentRead`, `ColorAttachmentWrite` | `ColorAttachment` |
| Depth-stencil attachment test and write | `DepthStencil` | `DepthStencilRead`, `DepthStencilWrite` | `DepthStencilWrite` |
| Depth-stencil attachment test without writes | `DepthStencil` | `DepthStencilRead` | `DepthStencilWrite` |
| Render-pass resolve target | `Resolve` | `Resolve` | `ColorAttachment` |
| Copy source | `Copy` | `CopyRead` | `CopySrc`, `General` |
| Copy destination, `fillBuffer` | `Copy` | `CopyWrite` | `CopyDst`, `General` |
| `clearTexture` | `Copy` | `CopyWrite` | `CopyDst` |
| Presentation ([presentation](11-presentation.md)) | `None` | `None` | `Present` |

Accesses of one render pass to its own attachments are ordered by the pass, and accesses within
one draw or dispatch are the shader's own business; every other pair of uses of the same memory
in which at least one writes, or between which a texture changes layout, needs a barrier. The
upscaler states its own uses in [upscaling](12-upscaling.md).

## Vocabulary

A combined bit, such as `Stage::AllGraphics`, lowers on every backend to the union of the mappings
of its member bits.

Rules marked *unverified* rest on D3D12 queue-type restrictions of enhanced barriers as recalled,
not as recorded in the research notes. They are kept as conservative rules until conformance
checks them on D3D12.

### `Stage`

```cpp
enum class Stage : uint32_t {
    None            = 0,
    IndirectArgs    = 1u << 0,
    IndexInput      = 1u << 1,
    VertexShader    = 1u << 2,
    TaskShader      = 1u << 3,
    MeshShader      = 1u << 4,
    FragmentShader  = 1u << 5,
    ComputeShader   = 1u << 6,
    ColorAttachment = 1u << 7,
    DepthStencil    = 1u << 8,
    Copy            = 1u << 9,
    Resolve         = 1u << 10,
    AllGraphics     = IndirectArgs | IndexInput | VertexShader | TaskShader | MeshShader |
                      FragmentShader | ColorAttachment | DepthStencil | Resolve,
    All             = (1u << 11) - 1,
};
// operator|, operator& and operator~ are defined for Stage.
```

A bit set of pipeline stages, modelled on D3D12 enhanced barriers and Vulkan synchronization2.

- `None`: no stage. As a source it means no earlier work; as a destination, no later work.
- `IndirectArgs`: reading indirect argument records and draw counts.
- `IndexInput`: fetching indices for indexed draws.
- `VertexShader`, `TaskShader`, `MeshShader`, `FragmentShader`, `ComputeShader`: execution of
  that shader kind.
- `ColorAttachment`: color attachment loads, blending and stores, including load-operation clears.
- `DepthStencil`: depth and stencil tests, writes and load-operation clears.
- `Copy`: every copy command, `fillBuffer` and `clearTexture`, on whatever engine the backend
  runs them.
- `Resolve`: the multisample resolve at the end of a render pass.
- `AllGraphics`: every stage a render pass and its draws use.
- `All`: every stage.

#### Valid usage
- `VU-rojo-sync-0001` A barrier recorded on a `QueueType::Compute` list names no stage bits other
  than `IndirectArgs`, `ComputeShader`, `Copy` and `All`, and one recorded on a `QueueType::Copy`
  list none other than `Copy` and `All` (*unverified*).

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| `None` | no stage: an execution-only barrier is dropped | `D3D12_BARRIER_SYNC_NONE`; as the source of a buffer barrier or of a texture barrier from `Undefined`, the destination's sync, or `SYNC_ALL` when the destination names no stage either, and as the source of a texture barrier out of another layout, that layout's own scope, because GPU-based validation rejects `SYNC_NONE` before a resource an earlier command touched | `VK_PIPELINE_STAGE_2_NONE` |
| `IndirectArgs` | `MTL::StageDispatch`, `MTL::StageVertex`, `MTL::StageObject` and `MTL::StageMesh`: the stages that consume arguments, since Metal has no argument stage | `SYNC_EXECUTE_INDIRECT` | `DRAW_INDIRECT` |
| `IndexInput` | `MTL::StageVertex` | `SYNC_INDEX_INPUT` | `INDEX_INPUT` |
| `VertexShader` | `MTL::StageVertex` | `SYNC_VERTEX_SHADING` | `VERTEX_SHADER` |
| `TaskShader` | `MTL::StageObject` | `SYNC_VERTEX_SHADING` | `TASK_SHADER_EXT` |
| `MeshShader` | `MTL::StageMesh` | `SYNC_VERTEX_SHADING` | `MESH_SHADER_EXT` |
| `FragmentShader` | `MTL::StageFragment` | `SYNC_PIXEL_SHADING` | `FRAGMENT_SHADER` |
| `ComputeShader` | `MTL::StageDispatch` | `SYNC_COMPUTE_SHADING` | `COMPUTE_SHADER` |
| `ColorAttachment` | `MTL::StageFragment` | `SYNC_RENDER_TARGET` | `COLOR_ATTACHMENT_OUTPUT` |
| `DepthStencil` | `MTL::StageFragment` | `SYNC_DEPTH_STENCIL` | `EARLY_FRAGMENT_TESTS \| LATE_FRAGMENT_TESTS` |
| `Copy` | `MTL::StageBlit \| MTL::StageDispatch \| MTL::StageFragment`: blits, the backend kernels of `fillBuffer` and `clearTexture`, and a `clearTexture` run as a render-pass load action | `SYNC_COPY`, plus `SYNC_COMPUTE_SHADING` for the `fillBuffer` kernel and `SYNC_RENDER_TARGET`, `SYNC_DEPTH_STENCIL` or `SYNC_CLEAR_UNORDERED_ACCESS_VIEW` for `clearTexture`, which the backend brackets with transitions from `CopyDst` into the render-target, depth-stencil-write or unordered-access layout and back; on a `Compute` list without `SYNC_RENDER_TARGET` and `SYNC_DEPTH_STENCIL`, on a `Copy` list `SYNC_COPY` alone | `COPY \| CLEAR` |
| `Resolve` | `MTL::StageFragment` (store-action resolve) | `SYNC_RENDER_TARGET \| SYNC_RESOLVE`: a resolve target stays in `LAYOUT_RENDER_TARGET` outside the backend's resolve transitions, whose render-target scope the barrier chains with | `COLOR_ATTACHMENT_OUTPUT` (render-pass resolve) |
| `AllGraphics` | the union of its bits: `MTL::StageDispatch \| MTL::StageVertex \| MTL::StageObject \| MTL::StageMesh \| MTL::StageFragment`, with `MTL::StageDispatch` from `IndirectArgs` for the argument-encoding kernel | `SYNC_DRAW \| SYNC_EXECUTE_INDIRECT \| SYNC_RESOLVE` | `ALL_GRAPHICS` |
| `All` | `MTL::StageAll` | `SYNC_ALL` | `ALL_COMMANDS` |
| Cost | native | cheap: `clearTexture` adds a backend transition into and out of its clear layout; every other stage is native | native |

### `Access`

```cpp
enum class Access : uint32_t {
    None                 = 0,
    IndirectArgs         = 1u << 0,
    IndexRead            = 1u << 1,
    ShaderRead           = 1u << 2,
    ShaderWrite          = 1u << 3,
    ColorAttachmentRead  = 1u << 4,
    ColorAttachmentWrite = 1u << 5,
    DepthStencilRead     = 1u << 6,
    DepthStencilWrite    = 1u << 7,
    CopyRead             = 1u << 8,
    CopyWrite            = 1u << 9,
    Resolve              = 1u << 10,
};
// operator|, operator& and operator~ are defined for Access.
```

A bit set of memory accesses. Each bit is performed at the stages named after it.

- `None`: no memory access; the side orders execution only.
- `IndirectArgs`: reads of argument records and counts, at `IndirectArgs`.
- `IndexRead`: index fetch, at `IndexInput`.
- `ShaderRead`: shader reads through an `Addr`, a view or a root block, at any shader stage.
- `ShaderWrite`: shader writes and atomics through an `Addr` or a `Storage` view, at any shader
  stage.
- `ColorAttachmentRead`, `ColorAttachmentWrite`: attachment loads and blending, and stores and
  clears, at `ColorAttachment`.
- `DepthStencilRead`, `DepthStencilWrite`: tests, and writes and clears, at `DepthStencil`.
- `CopyRead`, `CopyWrite`: copy sources, and copy, fill and clear destinations, at `Copy`.
- `Resolve`: writes to a resolve target, at `Resolve`.

`AllGraphics` and `All` perform every access their stages perform.

#### Valid usage
- `VU-rojo-sync-0002` Every access named on one side of a barrier is performed by at least one
  stage named on the same side.
- `VU-rojo-sync-0003` A side whose stage is `None` names access `None`.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Lowering | barriers carry no access: any access other than `None` selects `MTL4::VisibilityOptionDevice` | `D3D12_BARRIER_ACCESS` bits, less those the list's queue never performs, which a side naming `Stage::All` may name: a `Copy` list keeps copy accesses, a `Compute` list drops render-target, depth-stencil, resolve and index accesses, and a side left with none is `ACCESS_NO_ACCESS`; a side's sync then keeps only the bits compatible with one of its access bits (the `Copy` stage into `CopyDst` is `SYNC_COPY`), widening to `SYNC_ALL` if none is, and a side with no access keeps its sync but `SYNC_CLEAR_UNORDERED_ACCESS_VIEW`; a global barrier whose sides no one layout admits, which the layer rejects, drops its render-target and depth-stencil accesses, which only a texture in their layout performs and the texture barrier out of it orders: each side keeps the scopes it named, or `SYNC_ALL` when its remaining accesses do not run in every one of them, and each such layout whose accesses both sides name gets a global barrier of its own; a texture side with no access in a layout other than `Undefined`, which the layer accepts only with `SYNC_NONE`, names the accesses that layout admits that its sync runs, or all of them with `SYNC_ALL` when its sync runs none or runs scopes they do not | `VK_ACCESS_2` bits |
| `None` | execution-only barrier | `ACCESS_NO_ACCESS` | `NONE` |
| `IndirectArgs` | `Device` visibility | `ACCESS_INDIRECT_ARGUMENT` | `INDIRECT_COMMAND_READ` |
| `IndexRead` | `Device` visibility | `ACCESS_INDEX_BUFFER` | `INDEX_READ` |
| `ShaderRead` | `Device` visibility | `ACCESS_SHADER_RESOURCE \| ACCESS_CONSTANT_BUFFER` on global and buffer barriers; on a texture barrier `ACCESS_SHADER_RESOURCE`, or `ACCESS_UNORDERED_ACCESS` for `Storage` view reads when that side's layout is `ShaderWrite` | `SHADER_READ` |
| `ShaderWrite` | `Device` visibility | `ACCESS_UNORDERED_ACCESS` | `SHADER_WRITE` |
| `ColorAttachmentRead`, `ColorAttachmentWrite` | `Device` visibility | `ACCESS_RENDER_TARGET` | `COLOR_ATTACHMENT_READ`, `COLOR_ATTACHMENT_WRITE` |
| `DepthStencilRead`, `DepthStencilWrite` | `Device` visibility | `ACCESS_DEPTH_STENCIL_READ`, `ACCESS_DEPTH_STENCIL_WRITE` | `DEPTH_STENCIL_ATTACHMENT_READ`, `DEPTH_STENCIL_ATTACHMENT_WRITE` |
| `CopyRead`, `CopyWrite` | `Device` visibility | `ACCESS_COPY_SOURCE`, `ACCESS_COPY_DEST`; a `clearTexture` destination is cleared with `ACCESS_RENDER_TARGET`, `ACCESS_DEPTH_STENCIL_WRITE` or `ACCESS_UNORDERED_ACCESS` inside the backend's transitions; a `CopyWrite` on a global barrier, or on a buffer barrier of a `ShaderWrite` buffer, adds `ACCESS_UNORDERED_ACCESS` for the `fillBuffer` kernel, except on a `Copy` list, which runs no fill | `TRANSFER_READ`, `TRANSFER_WRITE` |
| `Resolve` | `Device` visibility | `ACCESS_RENDER_TARGET`: a resolve target is in `ColorAttachment`, and only the backend's transitions around the resolve name `ACCESS_RESOLVE_DEST` | `COLOR_ATTACHMENT_WRITE` |
| Cost | native | native | native |

### `Layout`

```cpp
enum class Layout : uint8_t {
    Undefined, General, ShaderRead, ShaderWrite, ColorAttachment,
    DepthStencilWrite, DepthStencilRead, CopySrc, CopyDst, Present,
};
```

The layout of a texture subresource. The set is the D3D12 enhanced-barrier core and maps onto
Vulkan image layouts, where `General` and `ShaderWrite` both become `GENERAL`; Metal 4 has no
layouts and ignores them. Buffers have no layout. The caller states both the layout a subresource
is in and the one it moves to; RojoRHI never infers either. A new texture starts `Undefined`, or
`ShaderRead` when it is created with initial data
([`Device::createTexture`](04-textures-and-heaps.md#devicecreatetexture)). Depth and stencil share
one layout.

- `Undefined`: contents are undefined. Valid only as the layout before a barrier, which discards
  the contents.
- `General`: shader reads, copy reads and copy writes other than `clearTexture`; the layout of
  a cross-queue handoff ([part 2](09-synchronization-part2.md)).
- `ShaderRead`: shader reads through `Sampled` views.
- `ShaderWrite`: shader reads and writes through `Storage` views.
- `ColorAttachment`: color attachment use, and the target of a render-pass resolve.
- `DepthStencilWrite`: depth-stencil attachment use with or without writes; a depth-stencil
  attachment is always in this layout, because read-only depth attachments are not part of 0.1.0
  ([`DepthAttachment`](07-command-recording.md#depthattachment)).
- `DepthStencilRead`: shader reads of a depth-stencil texture through `Sampled` views.
- `CopySrc`: copy source.
- `CopyDst`: copy, fill and clear destination.
- `Present`: handed to presentation; permits no other use.

#### Valid usage
- `VU-rojo-sync-0004` Every use of a texture subresource is permitted by the layout that subresource
  is in at that point in queue submission order, per the list above. Checked by the validation
  tracker.
- `VU-rojo-sync-0005` `Present` is used only for textures acquired from a
  [swapchain](11-presentation.md).
- `VU-rojo-sync-0006` A texture used by a `QueueType::Copy` list is in `General` (*unverified*).
  Checked by the validation tracker.
- `VU-rojo-sync-0007` A barrier recorded on a `QueueType::Compute` list uses no layouts other than
  `Undefined`, `General`, `ShaderRead`, `ShaderWrite`, `CopySrc` and `CopyDst` (*unverified*).
- `VU-rojo-sync-0026` A texture barrier recorded on a `QueueType::Copy` list has `General` as both
  `oldLayout` and `newLayout`: a copy queue takes no layout barrier, so a Copy list neither moves a
  texture to another layout nor discards one from `Undefined`.

#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| `Undefined` | ignored | `D3D12_BARRIER_LAYOUT_UNDEFINED` | `VK_IMAGE_LAYOUT_UNDEFINED` |
| `General` | ignored | `LAYOUT_COMMON`; on a `Copy` list, whose queue takes no layout barrier, every texture barrier, which keeps `General` on both sides (`VU-rojo-sync-0026`), lowers to a global barrier with its stages and accesses | `GENERAL` |
| `ShaderRead` | ignored | `LAYOUT_SHADER_RESOURCE`, valid on direct and compute queues | `SHADER_READ_ONLY_OPTIMAL` |
| `ShaderWrite` | ignored | `LAYOUT_UNORDERED_ACCESS`, valid on direct and compute queues | `GENERAL` |
| `ColorAttachment` | ignored | `LAYOUT_RENDER_TARGET`; a resolve target passes through `LAYOUT_RESOLVE_DEST` by a backend transition | `COLOR_ATTACHMENT_OPTIMAL` |
| `DepthStencilWrite` | ignored | `LAYOUT_DEPTH_STENCIL_WRITE` | `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` |
| `DepthStencilRead` | ignored | `LAYOUT_SHADER_RESOURCE`: the layout permits shader reads alone, and `LAYOUT_DEPTH_STENCIL_READ` admits no shader-resource access | `DEPTH_STENCIL_READ_ONLY_OPTIMAL` |
| `CopySrc` | ignored | `LAYOUT_COPY_SOURCE` | `TRANSFER_SRC_OPTIMAL` |
| `CopyDst` | ignored | `LAYOUT_COPY_DEST`; a `clearTexture` destination passes through the render-target, depth-stencil-write or unordered-access layout by a backend transition | `TRANSFER_DST_OPTIMAL` |
| `Present` | ignored | `LAYOUT_PRESENT` | `PRESENT_SRC_KHR` |
| Cost | native | cheap: a resolve target or a `clearTexture` destination adds a backend transition; every other layout is native | native |
