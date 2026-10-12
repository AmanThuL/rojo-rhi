# ADR 0010: The hardware floor

**Status**: Accepted (2026-10-12)

## Context

The v2 interface is address-first, bindless and explicitly synchronized, and every backend has to
express it without emulating large parts of it. Where a platform can only reach the model through
emulation, the cost lands on every call, so the useful question is which hardware and operating
system versions make the model native or cheap. The state of each API is recorded in two research
notes: [D3D12 and Vulkan](../research/2026-10-11-rhi-redesign/05-d3d12-vulkan-state.md) and
[Metal 4 and Slang](../research/2026-10-11-rhi-redesign/06-metal4-slang-state.md).

The findings that fix the floor:

- Metal 4 and 2026 Vulkan (descriptor heaps, push data, unified image layouts) are both close to
  the shader model of the interface. Metal's costs are 8-byte texture IDs, mandatory residency and
  the absence of inline root bytes.
- D3D12 sets the binding constraint. HLSL and DXIL have no pointers, render-target and depth
  textures need layout transitions, and depth-stencil state lives in the pipeline state object.
  Shader Model 6.6 heap indexing, enhanced barriers and root descriptors by GPU virtual address get
  most of the way there; the planned backend will emulate the rest.
- Legacy paths would double the surface. A legacy-barrier path on D3D12 or a Metal 3 path on Apple
  silicon would each need their own validation, tests and cost classes.

## Decision

The floor below is a requirement, not a preference. A device below it fails creation with an error;
the library has no reduced mode.

| Backend | Floor |
|---|---|
| Metal 4 | macOS 26 or later on Apple silicon, GPU family Apple7 (M1, A14) or newer; read-write texture tier 2 |
| D3D12 (planned) | Windows 11 x64; Agility SDK 1.619 or later, redistributed with the application; DirectX 12 Ultimate class hardware (feature level 12_2); Shader Model 6.6 heap indexing with resource binding tier 3; `TypedUAVLoadAdditionalFormats`; enhanced barriers; `DynamicDepthBiasSupported` |

Metal 4 argument tables, allocators, command barriers, view pools and residency sets are all
available at Apple7, as is mesh shading, so Apple7 is the lowest family on which the whole
interface runs natively. There is no Metal 3 fallback.

On D3D12 the hardware class is NVIDIA RTX 20 series and newer, AMD RDNA 2 and newer, and Intel Arc
A series and newer. Enhanced barriers are a driver-optional capability that the backend checks at
creation; there is no legacy `ResourceBarrier` path. The D3D12 backend does not exist: this floor
is fixed now so the interface is designed against it, and the backend is accepted only when it
passes the conformance suite unchanged ([0009](0009-conformance-and-valid-usage.md)).

`Device::capabilities()` reports the floor actually met and the limits that vary, among them
upload-heap availability. The `GPU_UPLOAD` heap on D3D12 (Windows 11 24H2 and Resizable BAR on
discrete GPUs) is optional: the fallback is the classic upload heap. Depth-bias values are dynamic
state; on D3D12 they are set through `ID3D12GraphicsCommandList9::RSSetDepthBias`, which needs
`DynamicDepthBiasSupported`, so creation checks that capability with the rest of the floor.
Per-vendor driver support for it is unverified, and the reference cites it as such.

The interface excludes work graphs, which are frozen at Shader Model 6.9, and cooperative vectors,
which are deprecated in favour of `linalg`. Per-draw pipeline choice on the GPU is deferred until
Work Lists or device-generated commands settle; GPU-driven work is indirect draws with a count.

Vulkan has no floor of its own and no code ([0008](0008-backends-and-vulkan-oracle.md) owns its
role). The oracle column in the API reference is judged against Vulkan 1.4 with descriptor heaps,
untyped pointers, device address commands and mesh shaders; unified image layouts are optional.

## Consequences

- The floor excludes Windows 10, Pascal, Maxwell and Volta GPUs, GTX 16 series cards (which lack
  feature level 12_2), RDNA 1 and Intel Macs. The research note estimates, from
  the Steam survey of September 2026, that roughly 85 to 90 percent of Steam GPUs meet Shader Model
  6.6 plus enhanced barriers and about 75 to 80 percent meet feature level 12_2; those figures are
  derived from the top of the survey table and are not measured.
- Whether Agility SDK 1.619 still loads on Windows 10 was not checked, and the floor does not
  depend on it. Windows 11 is required.
- Linux players will be reached through translation layers running the planned D3D12 backend; a
  native Vulkan backend would buy native tooling, not reach, and none is planned.
- Raising the floor needs a superseding ADR. Lowering it is a new emulation path with its own cost
  classes and tests, and needs one too.
- The cost classes in the API reference are stated against this floor: for example, layouts and
  stages are `cheap` on D3D12, where only a resolve target or a `clearTexture` destination adds a
  backend transition, and `DepthStencilState` is `native` on D3D12 because it lives in the
  pipeline state object. Making depth-stencil state dynamic would have cost hidden pipeline
  variants, which is why it stays in the pipeline.
