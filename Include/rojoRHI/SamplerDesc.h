//----------------------------------------------------------------------------------------------------------------------
/// @file SamplerDesc.h
/// @brief Declares the description of a sampler.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/CompareOp.h>
#include <rojoRHI/SamplerAddress.h>
#include <rojoRHI/SamplerFilter.h>

#include <cstdint>
#include <string_view>

namespace rojoRHI {
    /// Describes a sampler, for `Device::createSampler` and `Device::writeSampler`. `maxAnisotropy` is between 1 and 16
    /// (VU-rojo-texture-0029) and is above 1 only when `filter` and `mipFilter` are `Linear` (VU-rojo-texture-0030).
    struct SamplerDesc
    {
        /// Minification and magnification.
        SamplerFilter filter = SamplerFilter::Linear;
        /// Between mip levels.
        SamplerFilter mipFilter = SamplerFilter::Linear;
        /// Every axis.
        SamplerAddress address = SamplerAddress::Repeat;
        /// 1 disables anisotropic filtering.
        uint32_t maxAnisotropy = 1;
        /// True makes a comparison sampler, fetched only for depth comparisons.
        bool compareEnable = false;
        /// Used only when `compareEnable` is true.
        CompareOp compare = CompareOp::LessOrEqual;
        /// Empty selects a `rojorhi.`-prefixed default.
        std::string_view label;
    };
} // namespace rojoRHI
