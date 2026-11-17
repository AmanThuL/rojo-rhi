//----------------------------------------------------------------------------------------------------------------------
/// @file Formats.h
/// @brief Declares the per-class capability rules of chapter 15 and the table a device keeps.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <array>
#include <cstdint>

namespace rojoRHI::core {
    inline constexpr uint32_t kFormatCount = static_cast<uint32_t>(Format::RGB10A2Unorm) + 1;

    using FormatTable = std::array<FormatCapabilities, kFormatCount>;

    // Every capability the reference allows the format's class; a backend reports its intersection with the hardware, never
    // more.
    FormatCapabilities formatCeiling(Format format);

    FormatTable formatCeilings();

    struct FormatLayout
    {
        uint32_t blockWidth = 1;
        uint32_t blockHeight = 1;
        uint32_t blockBytes = 0; // Bytes of one block; `D32FloatS8` has none, because the reference gives it no packed size.
    };

    FormatLayout formatLayout(Format format);

    bool isBlockCompressed(Format format);

    bool isDepthFormat(Format format);

    // `capabilities.sampleCounts` sets bit log2(`samples`) for each supported count.
    bool samplesSupported(uint32_t samples, const FormatCapabilities& capabilities);

    bool hasStencilPlane(Format format);

    Format srgbCounterpart(Format format);

    struct TileExtent
    {
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t depth = 0; // Texels along z; 1 for every type but `Tex3D`.
    };

    // The D3D12 standard swizzle tile shapes: 64 KiB per tile, the extent following the bits per texel, or per block when
    // compressed; a one-dimensional type tiles as a 64 KiB row. A backend without tile shapes of its own registers it with
    // `DeviceCore::setTileExtents`.
    TileExtent standardTileExtent(Format format, TextureType type);

    void checkQueryFormat(Format format);
} // namespace rojoRHI::core
