//----------------------------------------------------------------------------------------------------------------------
/// @file TemporalScaler.h
/// @brief Declares vendor-neutral temporal reconstruction capability and frame inputs.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include "RHI/Texture.h"

#include <cstdint>
#include <string_view>

namespace lmx::rhi {

/// Device support for a temporal reconstruction algorithm, fixed for the device lifetime.
struct TemporalScalerSupport {
    bool available = false;     ///< Whether the device offers temporal reconstruction.
    float minInputScale = 0.0f; ///< Minimum content-to-output extent ratio, per axis.
    float maxInputScale = 0.0f; ///< Maximum content-to-output extent ratio, per axis.
    std::string_view name;      ///< Device-lifetime algorithm display name; empty when unavailable.
};

/// Optional device features, fixed at device creation.
struct DeviceCapabilities {
    TemporalScalerSupport temporalScaler; ///< Vendor temporal reconstruction support.
};

/// Immutable texture capacities, formats and active-content range for a temporal scaler.
struct TemporalScalerDesc {
    uint32_t inputWidth = 0;   ///< Allocated width of every input except exposure, in texels.
    uint32_t inputHeight = 0;  ///< Allocated height of every input except exposure, in texels.
    uint32_t outputWidth = 0;  ///< Reconstructed output width in texels.
    uint32_t outputHeight = 0; ///< Reconstructed output height in texels.
    Format colorFormat = Format::RGBA16Float;  ///< Pre-exposed scene-linear color format.
    Format depthFormat = Format::D32Float;     ///< Input depth format.
    Format motionFormat = Format::RG16Float;   ///< Two-channel motion-vector format.
    Format reactiveFormat = Format::R8Unorm;   ///< Normalized reactive-weight format.
    Format outputFormat = Format::RGBA16Float; ///< Reconstructed scene-linear color format.
    float minInputScale = 0.0f; ///< Minimum content-to-output ratio, within device support.
    float maxInputScale = 0.0f; ///< Maximum content-to-output ratio, within device support.
    std::string_view label;     ///< Diagnostic object label, copied at creation.
};

/// Device-owned reconstruction object with private history; in-flight destruction is safe.
class TemporalScaler {
public:
    /// Releases ownership; backend storage retires after every encoded frame finishes.
    virtual ~TemporalScaler() = default;
};

/// One reconstruction frame. Input formats and capacities must match the scaler descriptor.
struct TemporalScaleParams {
    Texture* color = nullptr;    ///< Pre-exposed scene-linear color input.
    Texture* depth = nullptr;    ///< This frame's depth input.
    Texture* motion = nullptr;   ///< Motion input whose scaled vectors point to previous positions.
    Texture* reactive = nullptr; ///< Reactive weight, with one requesting no accumulation.
    /// Sampled 1x1 R16Float working-exposure multiplier for input color.
    Texture* exposure = nullptr;
    Texture* output = nullptr; ///< Writable reconstructed output at the descriptor's output extent.
    uint32_t inputContentWidth = 0;  ///< Active origin-anchored input rectangle width.
    uint32_t inputContentHeight = 0; ///< Active origin-anchored input rectangle height.
    float jitterOffsetX = 0.0f;      ///< Horizontal raster jitter in input texels.
    float jitterOffsetY = 0.0f;      ///< Vertical raster jitter in input texels, positive down.
    float motionScaleX = 1.0f; ///< Converts stored horizontal motion to previous-position pixels.
    float motionScaleY = 1.0f; ///< Converts stored vertical motion to previous-position pixels.
    float preExposure = 1.0f;  ///< Positive fixed multiplier already applied to color input.
    bool reset = false;        ///< Discards the scaler's private history before this frame.
    bool reversedDepth = true; ///< Whether larger depth values represent nearer geometry.
    std::string_view label;    ///< Pass capture and GPU-timing label, copied during encoding.
};

} // namespace lmx::rhi
