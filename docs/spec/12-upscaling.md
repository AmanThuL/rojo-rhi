# 12. Upscaling

**Status**: Accepted
**API version**: 0.1.0

An upscaler reconstructs a high-resolution image from a jittered low-resolution frame, its depth,
its motion and the upscaler's private history. The interface mirrors the v1 temporal scaler and
fronts several vendor implementations behind one descriptor. Vendor SDKs and their licensing are
backend business: public headers name no vendor type, and an implementation either is compiled
into the backend or is not. `Device::capabilities().upscalers` ([device](02-device.md)) lists the
kinds the device can create.

On Metal 4 the implementation is MetalFX, recorded between encoders. The planned D3D12 backend
will ship FSR as its always-available implementation and compile DLSS and XeSS in when their SDKs
are present at build time. `Upscaler` is a resource handle ([fundamentals](01-fundamentals.md))
released with `Device::destroy`, deferred like any resource. Inputs and outputs are ordered by
ordinary barriers ([synchronization](09-synchronization.md)); the implementation's own resources
are synchronized by the backend.

**Unverified identifiers.** The DLSS (NGX), FSR (FidelityFX) and XeSS identifiers in this
chapter's mapping tables, and the FidelityFX SDK's Vulkan support, are unverified against the SDK
headers; cells that name them say so. They are confirmed when the backend integrates each SDK.

### `UpscalerKind`
```cpp
enum class UpscalerKind : uint8_t { Auto, MetalFX, DLSS, FSR, XeSS };
```

| Enumerator | Meaning |
|---|---|
| `Auto` | The backend's preferred available kind: MetalFX on Metal 4; on D3D12 the device vendor's own implementation (DLSS on NVIDIA, XeSS on Intel) when compiled in and supported, otherwise FSR |
| `MetalFX` | Apple MetalFX temporal scaler |
| `DLSS` | NVIDIA DLSS super resolution |
| `FSR` | AMD FidelityFX Super Resolution, temporal |
| `XeSS` | Intel XeSS super resolution |

Requesting a kind that is not compiled in, or that the device or driver refuses, makes
`Device::createUpscaler` return an error; it is never a silent substitution.

#### Valid usage
- None.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| `MetalFX` | `MTL4FX::TemporalScaler` | impossible | impossible |
| `DLSS` | impossible | NGX DLSS feature, when its SDK is compiled in (unverified) | NGX DLSS feature through its Vulkan entry points (unverified) |
| `FSR` | impossible: the FidelityFX SDK has no Metal backend | FidelityFX upscale context, always compiled in (unverified) | FidelityFX upscale context through a Vulkan backend (unverified: Vulkan support of the current FidelityFX SDK) |
| `XeSS` | impossible | XeSS context, when its SDK is compiled in (unverified) | XeSS context through its Vulkan entry points (unverified) |
| Cost | native: MetalFX only; every other kind fails creation | native: FSR always; DLSS and XeSS when compiled in | native: FSR, DLSS and XeSS through their Vulkan SDKs |

### `UpscalerFlags`
```cpp
enum class UpscalerFlags : uint32_t {
    None = 0, HDR = 1u << 0, ReversedDepth = 1u << 1, AutoExposure = 1u << 2,
};
```
A bit set fixed at creation; the usual bitwise operators apply.

| Flag | Meaning |
|---|---|
| `HDR` | Color input is high dynamic range scene-linear, not display-referred |
| `ReversedDepth` | Larger depth values are nearer the camera |
| `AutoExposure` | The implementation computes exposure itself; `UpscaleParams::exposure` is ignored |

#### Valid usage
- None.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| `HDR` | no descriptor flag: MetalFX takes the scene-linear input as given | unverified: FSR `FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE`; DLSS `NVSDK_NGX_DLSS_Feature_Flags_IsHDR`; XeSS without `XESS_INIT_FLAG_LDR_INPUT_COLOR` | unverified: the same SDK flags |
| `ReversedDepth` | `depthReversed` | unverified: FSR `FFX_UPSCALE_ENABLE_DEPTH_INVERTED`; DLSS `NVSDK_NGX_DLSS_Feature_Flags_DepthInverted`; XeSS `XESS_INIT_FLAG_INVERTED_DEPTH` | unverified: the same SDK flags |
| `AutoExposure` | `autoExposureEnabled` | unverified: FSR `FFX_UPSCALE_ENABLE_AUTO_EXPOSURE`; DLSS `NVSDK_NGX_DLSS_Feature_Flags_AutoExposure`; XeSS `XESS_INIT_FLAG_ENABLE_AUTOEXPOSURE` | unverified: the same SDK flags |
| Cost | native | native | native |

### `UpscalerDesc`
```cpp
struct UpscalerDesc {
    UpscalerKind kind = UpscalerKind::Auto;
    Extent2D input, output;
    Format color = Format::RGBA16Float, depth = Format::D32Float, motion = Format::RG16Float;
    Format outputFormat = Format::RGBA16Float;
    UpscalerFlags flags = UpscalerFlags::None;
    std::string_view label;
};
```
`Extent2D` is defined in [fundamentals](01-fundamentals.md#extent2d).

| Field | Meaning |
|---|---|
| `kind` | Implementation to create |
| `input` | Allocated extent of the color, depth, motion and reactive inputs, in texels |
| `output` | Extent of the reconstructed output, in texels |
| `color` | Format of the color input |
| `depth` | Format of the depth input |
| `motion` | Format of the two-channel motion input |
| `outputFormat` | Format of the reconstructed output |
| `flags` | Creation-time behaviour |
| `label` | Diagnostic label, copied at creation; defaults to `rojorhi.upscaler.unnamed` |

The active content inside `input` may shrink per frame through `UpscaleParams::inputContent`, so
one upscaler serves dynamic resolution without re-creation.

#### Valid usage
- `VU-rojo-upscale-0001` `input.width` and `input.height` are greater than zero.
- `VU-rojo-upscale-0002` `output.width` is at least `input.width`, and `output.height` is at least
  `input.height`.
- `VU-rojo-upscale-0003` `color` and `outputFormat` are color formats, `depth` is a depth format,
  and `motion` is a two-channel floating-point format.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Descriptor | `MTLFX::TemporalScalerDescriptor` | unverified: FSR `ffxCreateContextDescUpscale`; DLSS `NVSDK_NGX_DLSS_Create_Params`; XeSS `xess_d3d12_init_params_t` | unverified: FSR `ffxCreateContextDescUpscale`; DLSS `NVSDK_NGX_DLSS_Create_Params`; XeSS `xess_vk_init_params_t` |
| Dynamic content | `inputContentPropertiesEnabled`, with the scale range derived from `input`, `output` and device support | unverified: FSR `FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION`; DLSS and XeSS take a per-frame input size | the same as D3D12 |
| Cost | native | native | native |

### `Device::createUpscaler`
```cpp
Result<Upscaler> Device::createUpscaler(const UpscalerDesc& desc);
```
Creates the implementation `desc.kind` selects, with its private history. Creation returns an error
when the kind is unavailable on this device, when the implementation rejects the extent ratio or
a format, or when the platform refuses the object. Creation is free-threaded and synchronous.

#### Valid usage
- None.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | `MTLFX::TemporalScalerDescriptor::newTemporalScaler(device, compiler)` | unverified: FSR `ffxCreateContext`; DLSS `NVSDK_NGX_D3D12_CreateFeature`; XeSS `xessD3D12CreateContext` and `xessD3D12Init` | unverified: FSR `ffxCreateContext`; DLSS `NVSDK_NGX_VULKAN_CreateFeature`; XeSS `xessVKCreateContext` and `xessVKInit` |
| Cost | native | cheap: DLSS feature creation records into a backend-owned command list | cheap: DLSS feature creation records into a backend-owned command buffer |

### `UpscaleParams`
```cpp
struct UpscaleParams {
    Texture color, depth, motion, output;
    Texture exposure = {}, reactive = {};
    Extent2D inputContent = {};
    float jitterX = 0.0f, jitterY = 0.0f;
    float motionScaleX = 1.0f, motionScaleY = 1.0f;
    float preExposure = 1.0f;
    float sharpness = 0.0f;
    bool reset = false;
    std::string_view label;
};
```

| Field | Meaning |
|---|---|
| `color` | Jittered color input; scene-linear and pre-exposed by `preExposure` with `UpscalerFlags::HDR` |
| `depth` | This frame's depth |
| `motion` | Per-pixel motion; scaled by `motionScaleX` and `motionScaleY`, it points from the current position to the previous one, in input pixels |
| `output` | Reconstructed result at the descriptor's `output` extent |
| `exposure` | Optional 1x1 exposure multiplier for the color input; null means 1.0; ignored with `UpscalerFlags::AutoExposure` |
| `reactive` | Optional per-pixel weight; 1.0 asks for no history accumulation; null means none |
| `inputContent` | Active origin-anchored rectangle of the inputs; zero in both axes means the whole `input` extent |
| `jitterX`, `jitterY` | Sub-pixel raster jitter of this frame, in input texels, positive down |
| `motionScaleX`, `motionScaleY` | Converts stored motion to input pixels |
| `preExposure` | Positive multiplier already applied to the color input |
| `sharpness` | Sharpening strength from 0 to 1; implementations without a sharpening pass ignore it |
| `reset` | Discards the history before this frame, as after a camera cut |
| `label` | Debug-group label of the operation in captures and scope timings, copied while recording |

#### Valid usage
- `VU-rojo-upscale-0004` `preExposure` is greater than zero.
- `VU-rojo-upscale-0005` `sharpness` is at least 0 and at most 1.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Inputs | `colorTexture`, `depthTexture`, `motionTexture`, `exposureTexture`, `reactiveMaskTexture`, `outputTexture` | unverified: FSR `ffxDispatchDescUpscale`; DLSS `NVSDK_NGX_D3D12_DLSS_Eval_Params`; XeSS `xess_d3d12_execute_params_t` | unverified: the Vulkan counterparts of the same SDK structures |
| Frame values | `inputContentWidth` and `inputContentHeight`, `jitterOffsetX` and `jitterOffsetY`, `motionVectorScaleX` and `motionVectorScaleY`, `preExposure`, `reset` | render size, jitter offset, motion vector scale, pre-exposure, reset and sharpness fields of the same structures | the same as D3D12 |
| Sharpness | ignored: the temporal scaler has no sharpening pass | honoured by FSR; ignored by DLSS and XeSS | the same as D3D12 |
| Reactive mask | enabled at creation; a null `reactive` binds a backend-owned cleared mask | unverified: FSR reactive mask; DLSS bias-current-color mask; XeSS responsive pixel mask | the same as D3D12 |
| Cost | native | native | native |

### `CommandList::upscale`
```cpp
void CommandList::upscale(Upscaler upscaler, const UpscaleParams& params);
```
Records one reconstruction. The implementation reads the inputs and its history and writes
`output`, as compute work. Before the call, the caller's barriers make the inputs readable in
`Layout::ShaderRead` and the output writable in `Layout::ShaderWrite`; after it, a barrier from
shader write orders later uses of the output. The output needs `TextureUsage::RenderTarget` as
well as `TextureUsage::Storage` because MetalFX can require render-target usage of its output. The
operation forms its own debug group named `params.label`, defaulted to `rojorhi.upscale.unnamed`.

#### Valid usage
- `VU-rojo-upscale-0006` The command list was acquired for `QueueType::Graphics`.
- `VU-rojo-upscale-0007` No render pass is open on the command list.
- `VU-rojo-upscale-0008` The upscaler is used by at most one `CommandList::upscale` per frame.
- `VU-rojo-upscale-0009` `color`, `depth`, `motion` and `output` are not null.
- `VU-rojo-upscale-0010` `color`, `depth` and `motion` have the formats of the upscaler's
  descriptor and an extent of exactly its `input`.
- `VU-rojo-upscale-0011` `output` has the descriptor's `outputFormat` and an extent of exactly
  its `output`, and is none of the input textures.
- `VU-rojo-upscale-0012` A non-null `exposure` is a 1x1 `Format::R16Float` texture; a non-null
  `reactive` is a `Format::R8Unorm` texture with an extent of exactly the descriptor's `input`.
- `VU-rojo-upscale-0013` Every input was created with `TextureUsage::Sampled`.
- `VU-rojo-upscale-0014` `output` was created with `TextureUsage::Storage` and
  `TextureUsage::RenderTarget`.
- `VU-rojo-upscale-0015` `inputContent` is zero in both axes, or its width and height are each at
  least one texel and at most the descriptor's `input` in that axis.
- `VU-rojo-upscale-0016` At execution every input is in `Layout::ShaderRead` and `output` is in
  `Layout::ShaderWrite`. Checked by the validation tracker.
#### Mapping
| | Metal 4 | D3D12 | Vulkan |
|---|---|---|---|
| Call | set the scaler's inputs, then `MTL4FX::TemporalScaler::encodeToCommandBuffer` between encoders | unverified: FSR `ffxDispatch`; DLSS `NGX_D3D12_EVALUATE_DLSS_EXT`; XeSS `xessD3D12Execute` | unverified: FSR `ffxDispatch`; DLSS `NGX_VULKAN_EVALUATE_DLSS_EXT`; XeSS `xessVKExecute` |
| Cost | cheap: ends the open compute encoder, and an `MTL::Fence` carries barrier visibility into MetalFX's own encoders | cheap: the backend rebinds its global root signature and descriptor heaps after the vendor call | cheap: the backend rebinds its descriptor heap and push data after the vendor call |
