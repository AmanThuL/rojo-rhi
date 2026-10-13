//----------------------------------------------------------------------------------------------------------------------
/// @file SamplerFilter.h
/// @brief Declares the texel filter of a sampler.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// How a sampler picks or blends texels.
    enum class SamplerFilter : uint8_t
    {
        Nearest, ///< The nearest texel or mip level.
        Linear,  ///< A linear blend.
    };
} // namespace rojoRHI
