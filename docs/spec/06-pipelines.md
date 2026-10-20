# 06. Pipelines

**Status**: Accepted
**API version**: 0.1.0

A pipeline holds only the state that at least one backend must bake at creation. There is no
vertex-input state, because vertices are always pulled by the shader, and no pipeline layout:
Metal 4 and the planned D3D12 backend use one fixed global layout, an argument table on Metal 4
and one global root signature on D3D12, and the Vulkan oracle would use one descriptor heap pair
(see [shader ABI](05-shader-abi.md)). Metal 4 columns describe the implemented backend; D3D12
columns describe the planned backend; Vulkan columns are the design oracle. Cost classes are
defined in [conventions](00-conventions.md).

- **Depth-stencil and blend stay in the pipeline.** The No Graphics API proposal makes both
  dynamic, and Metal 4 could follow cheaply, but on D3D12 both are part of the pipeline state
  object, so dynamic depth-stencil or blend state would mean hidden pipeline variants and a hash
  lookup per draw. The portable interface takes the D3D12 constraint; the mapping tables say so.
- **Dynamic state** is the small set every backend sets on the command list: viewport, scissor,
  stencil reference, blend constants, exact topology within the pipeline's class, and depth-bias
  values. Those entries, compute pipelines, `createPipeline` and the pipeline cache are in
  [pipelines: creation and dynamic state](06-pipelines-part2.md).
- **No run-time specialization.** Permutations are compiled offline through Slang generics or
  preprocessor definitions. D3D12 has no specialization constants. Metal 4's unspecialized color
  state is a backend-internal optimization, not interface.
- **Stutter control.** `createPipeline` is free-threaded and synchronous; RojoRHI creates no
  threads, so a consumer parallelizes creation on its own job system. A `PipelineCache` blob saved
  by the consumer and passed back at device creation skips recompilation. Counters report pipelines
  created, cache hits and misses, and compile time ([diagnostics](14-diagnostics.md)); the budget
  for hidden pipeline variants is zero on every backend.

### `ShaderEntry`

```cpp
struct ShaderEntry { ShaderLibrary library; std::string_view name; };
```

Names one entry point of a loaded shader library. A default-constructed entry (null `library`)
means the stage is absent. `name` is copied by `createPipeline` and need not outlive the call.

#### Valid usage

- `VU-rojo-pipeline-0001` When the stage is present, `library` is a live `ShaderLibrary` and `name`
  is not empty.
- `VU-rojo-pipeline-0002` When the stage is present, `name` names an entry point of `library`
  whose shader stage is the stage of the descriptor field holding the entry.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::LibraryFunctionDescriptor` (`setLibrary`, `setName`) | `D3D12_SHADER_BYTECODE` of the entry's DXIL | `VkPipelineShaderStageCreateInfo` (module, `pName`) |
| Cost | native | native | native |

### `Device::loadShaderLibrary`

```cpp
Result<ShaderLibrary> Device::loadShaderLibrary(std::span<const std::byte> blob,
                                                std::string_view label);
```

Loads a precompiled shader library the xmake shader rule produced and stamped with
`ROJORHI_SHADER_ABI_VERSION` ([shader ABI](05-shader-abi.md)): on Metal 4 a RojoRHI shader library
around a metallib, on D3D12 a RojoRHI shader library around each entry's DXIL. A blob stamped with
another ABI version returns `ErrorCategory::ShaderAbiMismatch`; one not built for the active backend
(another magic, or no container) or malformed returns `ErrorCategory::DriverFailure`, instead of
loading a stale shader. RojoRHI never compiles shader source at run time. The bytes are consumed
before return.
Free-threaded. Destroyed through `Device::destroy` ([device](02-device.md)).

#### Valid usage

- `VU-rojo-pipeline-0003` `blob` is not empty.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::Device::newLibrary` from the metallib bytes after the header, each entry checked with `MTL::Library::newFunction` | parses the RojoRHI shader library and keeps each entry's DXIL; no native object | `vkCreateShaderModule` (SPIR-V) |
| Cost | native | cheap: blob parsed and copied | native |

### `BlendState`

```cpp
enum class BlendFactor : uint8_t {
    Zero, One, SrcColor, OneMinusSrcColor, SrcAlpha, OneMinusSrcAlpha, DstColor, OneMinusDstColor,
    DstAlpha, OneMinusDstAlpha, SrcAlphaSaturate, Constant, OneMinusConstant,
};
enum class BlendOp : uint8_t { Add, Subtract, ReverseSubtract, Min, Max };
struct BlendState {
    bool enable = false;
    BlendFactor srcColor = BlendFactor::One, dstColor = BlendFactor::Zero;   // fragment, attachment
    BlendOp colorOp = BlendOp::Add;                                          // color terms
    BlendFactor srcAlpha = BlendFactor::One, dstAlpha = BlendFactor::Zero;   // fragment, attachment
    BlendOp alphaOp = BlendOp::Add;                                          // alpha terms
};
```

Fixed-function blending for one color target, baked into the pipeline. With `enable` false the
fragment output is written unchanged. A `Src` factor reads the fragment's color or alpha and a `Dst`
factor the attachment's; `SrcAlphaSaturate` is the smaller of the fragment's alpha and 1 minus the
attachment's alpha. `Subtract` is source minus destination; `ReverseSubtract` is destination minus
source; `Min` and `Max` ignore the factors. `Constant` and `OneMinusConstant` read the values of
`CommandList::setBlendConstants`. Dual-source blending and logic operations are not part of 0.1.0.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | blend fields of `MTL4::RenderPipelineColorAttachmentDescriptor` | `D3D12_RENDER_TARGET_BLEND_DESC` in the PSO, `IndependentBlendEnable` set | `VkPipelineColorBlendAttachmentState` |
| Made dynamic | cheap: specialization of an unspecialized color state per blend state | costly: hidden PSO variants and a hash lookup per draw | native with extended dynamic state 3, which is optional |
| Cost | native | native | native |

### `ColorTarget`

```cpp
enum class ColorWriteMask : uint8_t { None = 0, R = 1, G = 2, B = 4, A = 8, All = 15 };  // bit set
struct ColorTarget { Format format; BlendState blend; ColorWriteMask writeMask = ColorWriteMask::All; };
```

One color output of a graphics pipeline: the attachment format, its blend state and the channels
written. Entry `i` of `GraphicsPipelineDesc::colors` matches color attachment `i` of the render pass
([command recording](07-command-recording.md)). `Format` and `FormatCapabilities` are defined in
[formats and limits](15-formats-and-limits.md); the format reports `colorAttachment`, and `blend`
when blending is enabled, as `VU-rojo-format-0003` requires.

#### Valid usage

- `VU-rojo-pipeline-0004` Retired in 0.1.0: `Device::formatCapabilities(format)` reports
  `colorAttachment`. `VU-rojo-format-0003` states every format capability once
  ([formats and limits](15-formats-and-limits.md)).
- `VU-rojo-pipeline-0005` Retired in 0.1.0: `blend.enable` is true only when
  `Device::formatCapabilities(format)` reports `blend`. `VU-rojo-format-0003` states it.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::RenderPipelineColorAttachmentDescriptor` (`setPixelFormat`, `setWriteMask`) | `RTVFormats[i]`, `RenderTargetWriteMask` | `VkPipelineRenderingCreateInfo` formats, `colorWriteMask` |
| Cost | native | native | native |

### `RasterState`

```cpp
enum class CullMode : uint8_t { None, Front, Back };               // the faces discarded
enum class FrontFace : uint8_t { CounterClockwise, Clockwise };   // the winding of a front face
enum class FillMode : uint8_t { Solid, Wireframe };               // filled triangles, or edges only
struct RasterState {
    CullMode cull = CullMode::None;
    FrontFace frontFace = FrontFace::CounterClockwise;
    FillMode fill = FillMode::Solid;
    bool depthClip = true;      // false clamps fragment depth instead of clipping
    bool depthBias = false;     // enables CommandList::setDepthBias values
    bool conservative = false;  // conservative rasterization
};
```

Rasterizer state baked into a graphics pipeline. With `depthBias` false, depth-bias values set on
the command list are ignored; with it true they apply ([dynamic state](06-pipelines-part2.md)).

#### Valid usage

- `VU-rojo-pipeline-0006` `conservative` is true only when `Capabilities::conservativeRaster` is
  true ([device](02-device.md)).

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::RenderCommandEncoder` state (`setCullMode`, `setFrontFacingWinding`, `setTriangleFillMode`, `setDepthClipMode`) replayed at `setPipeline` | `D3D12_RASTERIZER_DESC2` in the PSO; `depthBias` sets `D3D12_PIPELINE_STATE_FLAG_DYNAMIC_DEPTH_BIAS` | `VkPipelineRasterizationStateCreateInfo` |
| Conservative | impossible: no Metal API; reported absent | native: `ConservativeRaster` | native where `VK_EXT_conservative_rasterization` is present |
| Cost | cheap: a few encoder calls per `setPipeline` | native | native |

### `CompareOp`

```cpp
enum class CompareOp : uint8_t {
    Never, Less, Equal, LessOrEqual, Greater, NotEqual, GreaterOrEqual, Always,
};
```

The one comparison function of the interface: a test passes when `incoming op stored` holds, or
always (`Always`) or never (`Never`). Depth and stencil tests use it here; sampler comparison uses
it in `SamplerDesc` ([textures and heaps, part 2](04-textures-and-heaps-part2.md#samplerdesc)).

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::CompareFunction` | `D3D12_COMPARISON_FUNC` | `VkCompareOp` |
| Cost | native | native | native |

### `DepthStencilState`

```cpp
enum class StencilOp : uint8_t {
    Keep, Zero, Replace, IncrementClamp, DecrementClamp, Invert, IncrementWrap, DecrementWrap,
};
struct StencilFaceState {
    StencilOp fail = StencilOp::Keep, depthFail = StencilOp::Keep, pass = StencilOp::Keep;
    CompareOp compare = CompareOp::Always;   // the stencil comparison
};
struct DepthStencilState {
    bool depthTest = false, depthWrite = false;
    CompareOp depthCompare = CompareOp::GreaterOrEqual;   // reverse Z, matching clear depth 0
    bool stencilTest = false;
    uint8_t stencilReadMask = 0xFF, stencilWriteMask = 0xFF;   // compared bits, written bits
    StencilFaceState front, back;                              // operations per face orientation
};
```

Depth test, depth write, compare and stencil operations, baked into the pipeline. A face applies
`fail` when its stencil test fails, `depthFail` when the stencil test passes and the depth test
fails, and `pass` when both pass. `Replace` writes the reference set by
`CommandList::setStencilReference`; `IncrementClamp` and `DecrementClamp` saturate at the maximum
and at 0, `IncrementWrap` and `DecrementWrap` wrap around, and `Invert` inverts the bits. Depth
bounds testing is not part of 0.1.0.

#### Valid usage

- `VU-rojo-pipeline-0007` `depthWrite` is true only when `depthTest` is true.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL::DepthStencilDescriptor`, made into an `MTL::DepthStencilState` by `MTL::Device::newDepthStencilState` at pipeline creation, equal states sharing one object and a state testing neither depth nor stencil none, and set by `MTL4::RenderCommandEncoder::setDepthStencilState` at `setPipeline` | `D3D12_DEPTH_STENCIL_DESC2` in the PSO | `VkPipelineDepthStencilStateCreateInfo` |
| Made dynamic | native: a separate object | costly: hidden PSO variants and a hash lookup per draw | native: core dynamic state |
| Cost | cheap: one extra object set per `setPipeline` | native | native |

### `PrimitiveClass`

```cpp
enum class PrimitiveClass : uint8_t { Point, Line, Triangle };
```

The topology class baked into a graphics pipeline with a vertex stage. The exact topology within
the class is dynamic (`CommandList::setPrimitiveTopology`). A mesh pipeline ignores the class: its
mesh entry point declares its output topology.

#### Valid usage

- None.

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::RenderPipelineDescriptor::setInputPrimitiveTopology` (`MTL::PrimitiveTopologyClass`) | `PrimitiveTopologyType` | input-assembly topology with `VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY` |
| Cost | native | native | native |

### `GraphicsPipelineDesc`

```cpp
struct GraphicsPipelineDesc {
    ShaderEntry vertex;                       // or task and mesh entries
    ShaderEntry task, mesh;
    ShaderEntry fragment;                     // absent for depth-only pipelines
    PrimitiveClass primitive = PrimitiveClass::Triangle;   // exact topology is dynamic
    RasterState raster;
    DepthStencilState depthStencil;
    std::span<const ColorTarget> colors;
    Format depthFormat = Format::Undefined;   // Undefined: no depth attachment
    uint32_t samples = 1;                     // of every attachment
    std::string_view label;
};
```

Describes a vertex pipeline (`vertex`, optional `fragment`) or a mesh pipeline (optional `task`,
`mesh`, optional `fragment`). Spans and strings are copied by `createPipeline`. Every color format
and `depthFormat` report `samples` among their sample counts, and `depthFormat` reports
`depthAttachment`, as `VU-rojo-format-0003` requires.

#### Valid usage

- `VU-rojo-pipeline-0008` Exactly one of `vertex` and `mesh` is present.
- `VU-rojo-pipeline-0009` `task` is present only when `mesh` is present.
- `VU-rojo-pipeline-0010` `colors` holds at most `Limits::maxColorAttachments` entries.
- `VU-rojo-pipeline-0011` `depthFormat` is `Format::Undefined` or a depth or depth-stencil format;
  `depthStencil.depthTest` requires a depth component and `depthStencil.stencilTest` a stencil
  component in `depthFormat`.
- `VU-rojo-pipeline-0012` Retired in 0.1.0: `FormatCapabilities::sampleCounts` includes `samples`
  for every color format and for `depthFormat`. `VU-rojo-format-0003` states every format
  capability once ([formats and limits](15-formats-and-limits.md)).

#### Mapping

| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTL4::RenderPipelineDescriptor` or `MTL4::MeshRenderPipelineDescriptor`, rasterization disabled when there is neither a fragment entry nor a depth format; Metal runs a vertex entry without rasterization only when it returns nothing, so a vertex pipeline without a fragment entry that Metal refuses is compiled once more with the other setting; a mesh pipeline with color targets and a depth format and no fragment entry returns `ErrorCategory::DriverFailure` | pipeline state stream: the global root signature, VS and an empty input layout or AS and MS, PS when a fragment entry is present, blend, sample mask, `RASTERIZER2`, `DEPTH_STENCIL2`, topology type (vertex pipelines), RTV and DSV formats, sample description, flags | `VkGraphicsPipelineCreateInfo` with `VkPipelineRenderingCreateInfo`, no vertex input, descriptor-heap flag |
| Cost | native | native | native |
