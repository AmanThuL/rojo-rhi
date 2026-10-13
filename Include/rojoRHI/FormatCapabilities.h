//----------------------------------------------------------------------------------------------------------------------
/// @file FormatCapabilities.h
/// @brief Declares what a device supports for one format.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// What one device supports for one format, fixed for the lifetime of the device. A member is `true` only when every
    /// operation it names is supported for every texture type the device can create in that format; every value is at least
    /// the guaranteed capability the reference states.
    struct FormatCapabilities
    {
        bool sampled = false;         ///< Readable through a Sampled view.
        bool filterLinear = false;    ///< Linear minification and magnification filtering.
        bool storageRead = false;     ///< Readable through a Storage view.
        bool storageWrite = false;    ///< Writable through a Storage view.
        bool colorAttachment = false; ///< Usable as a ColorAttachment texture.
        bool blend = false;           ///< Blendable as a color attachment.
        bool depthAttachment = false; ///< Usable as a DepthAttachment texture.
        bool resolve = false;         ///< Usable as a ColorAttachment resolve target.
        bool sparse = false;          ///< Creatable with TextureUsage::Sparse.
        uint32_t sampleCounts = 1;    ///< Bit i set: 1 << i samples supported; bit 0 always set.
    };
} // namespace rojoRHI
