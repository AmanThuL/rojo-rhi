//----------------------------------------------------------------------------------------------------------------------
/// @file ValidateTemporalScaler.cpp
/// @brief Validates temporal reconstruction descriptors and per-frame inputs.
//----------------------------------------------------------------------------------------------------------------------
#include "RHI/Validate.h"

#include <cmath>
#include <string>

namespace lmx::rhi {
namespace {

//======================================================================================================================
Result<void> invalid(std::string message) {
    return std::unexpected(Error{ErrorCode::InvalidDesc, std::move(message)});
}

//======================================================================================================================
bool matches(const Texture* texture, uint32_t width, uint32_t height, Format format) {
    return texture != nullptr && texture->width() == width && texture->height() == height &&
           texture->format() == format && texture->mipLevels() == 1 && texture->arrayLayers() == 1;
}

} // namespace

//======================================================================================================================
Result<void> validate(const TemporalScalerDesc& desc, const TemporalScalerSupport& support) {
    if (desc.inputWidth == 0 || desc.inputHeight == 0 || desc.outputWidth == 0 ||
        desc.outputHeight == 0) {
        return invalid("TemporalScalerDesc extents must be greater than zero");
    }
    if (!support.available) {
        return invalid("TemporalScalerDesc requires available device support");
    }
    if (!std::isfinite(desc.minInputScale) || !std::isfinite(desc.maxInputScale) ||
        desc.minInputScale <= 0.0f || desc.minInputScale > desc.maxInputScale ||
        desc.minInputScale < support.minInputScale || desc.maxInputScale > support.maxInputScale) {
        return invalid("TemporalScalerDesc content scale range is outside device support");
    }
    if (static_cast<double>(desc.inputWidth) <
            desc.outputWidth * static_cast<double>(desc.maxInputScale) ||
        static_cast<double>(desc.inputHeight) <
            desc.outputHeight * static_cast<double>(desc.maxInputScale)) {
        return invalid("TemporalScalerDesc input capacity cannot hold the maximum content scale");
    }
    if (desc.colorFormat != Format::RGBA16Float || desc.depthFormat != Format::D32Float ||
        desc.motionFormat != Format::RG16Float || desc.reactiveFormat != Format::R8Unorm ||
        desc.outputFormat != Format::RGBA16Float) {
        return invalid("TemporalScalerDesc has an unsupported format combination");
    }
    return {};
}

//======================================================================================================================
Result<void> validateTemporalScale(const TemporalScalerDesc& desc,
                                   const TemporalScaleParams& params) {
    if (!matches(params.color, desc.inputWidth, desc.inputHeight, desc.colorFormat) ||
        !matches(params.depth, desc.inputWidth, desc.inputHeight, desc.depthFormat) ||
        !matches(params.motion, desc.inputWidth, desc.inputHeight, desc.motionFormat) ||
        !matches(params.reactive, desc.inputWidth, desc.inputHeight, desc.reactiveFormat) ||
        !matches(params.exposure, 1, 1, Format::R16Float) ||
        !matches(params.output, desc.outputWidth, desc.outputHeight, desc.outputFormat)) {
        return invalid(
            "temporalScale textures must be non-null and match descriptor formats and capacities");
    }
    if (params.output == params.color || params.output == params.depth ||
        params.output == params.motion || params.output == params.reactive ||
        params.output == params.exposure) {
        return invalid("temporalScale output must not alias an input texture");
    }
    const double scaleX = static_cast<double>(params.inputContentWidth) / desc.outputWidth;
    const double scaleY = static_cast<double>(params.inputContentHeight) / desc.outputHeight;
    if (params.inputContentWidth == 0 || params.inputContentHeight == 0 ||
        params.inputContentWidth > desc.inputWidth ||
        params.inputContentHeight > desc.inputHeight || scaleX < desc.minInputScale ||
        scaleX > desc.maxInputScale || scaleY < desc.minInputScale || scaleY > desc.maxInputScale) {
        return invalid(
            "temporalScale content rectangle is outside capacity or content scale range");
    }
    if (!std::isfinite(params.jitterOffsetX) || !std::isfinite(params.jitterOffsetY) ||
        !std::isfinite(params.motionScaleX) || !std::isfinite(params.motionScaleY) ||
        !std::isfinite(params.preExposure) || params.preExposure <= 0.0f) {
        return invalid(
            "temporalScale jitter, motion scale and positive preExposure must be finite");
    }
    return {};
}

} // namespace lmx::rhi
