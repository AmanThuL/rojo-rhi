//----------------------------------------------------------------------------------------------------------------------
/// @file SubresourceRange.h
/// @brief Declares the range of mips and array layers a view or a barrier covers.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// A `mipCount` that names every mip from `baseMip` through the last.
    inline constexpr uint32_t kAllMips = ~0u;

    /// A `layerCount` that names every layer from `baseLayer` through the last.
    inline constexpr uint32_t kAllLayers = ~0u;

    /// Names the mips and array layers a view or a texture barrier covers; the default covers the whole texture, and layers of
    /// cube types are faces. Neither count is 0 (VU-rojo-texture-0016), and the range lies inside the texture
    /// (VU-rojo-texture-0017).
    struct SubresourceRange
    {
        /// First mip.
        uint32_t baseMip = 0;
        /// Mips covered, or `kAllMips`.
        uint32_t mipCount = kAllMips;
        /// First layer.
        uint32_t baseLayer = 0;
        /// Layers covered, or `kAllLayers`.
        uint32_t layerCount = kAllLayers;
    };
} // namespace rojoRHI
