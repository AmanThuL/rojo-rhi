//----------------------------------------------------------------------------------------------------------------------
/// @file Formats.cpp
/// @brief Implements the per-class capability rules of chapter 15.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Formats.h"

#include "Base/Assert.h"
#include <algorithm>
#include <bit>

namespace rojoRHI::core {
    namespace {
        /// The classes whose rules differ: chapter 15 lets each have the capabilities below and forbids the rest.
        enum class FormatClass
        {
            None,            ///< Undefined: nothing.
            Color,           ///< Color formats: sampled, filtered, storage, color attachment, blend, resolve, multisampled.
            ColorNoStorage,  ///< sRGB color formats: as Color, without shader storage access.
            BlockCompressed, ///< BC formats: sampled and filtered only.
            Depth,           ///< Depth and depth-stencil formats: sampled, filtered, depth attachment, multisampled.
        };

        //==============================================================================================================
        FormatClass classOf(Format format)
        {
            switch (format)
            {
            case Format::Undefined: return FormatClass::None;
            case Format::RGBA8Unorm_sRGB: return FormatClass::ColorNoStorage;
            case Format::BC1Unorm:
            case Format::BC1Unorm_sRGB: return FormatClass::BlockCompressed;
            case Format::D32Float:
            case Format::D32FloatS8: return FormatClass::Depth;
            default: return FormatClass::Color;
            }
        }
    } // namespace

    //==================================================================================================================
    FormatCapabilities formatCeiling(Format format)
    {
        FormatCapabilities caps;
        const FormatClass formatClass = classOf(format);
        if (formatClass == FormatClass::None)
        {
            return caps;
        }
        caps.sampled = true;
        caps.filterLinear = true;
        caps.sparse = true;
        if (formatClass == FormatClass::BlockCompressed)
        {
            return caps;
        }
        caps.sampleCounts = 0b1111;
        if (formatClass == FormatClass::Depth)
        {
            caps.depthAttachment = true;
            return caps;
        }
        caps.colorAttachment = true;
        caps.blend = true;
        caps.resolve = true;
        if (formatClass == FormatClass::Color)
        {
            caps.storageRead = true;
            caps.storageWrite = true;
        }
        return caps;
    }

    //==================================================================================================================
    FormatTable formatCeilings()
    {
        FormatTable table;
        for (uint32_t value = 0; value < kFormatCount; ++value)
        {
            table[value] = formatCeiling(static_cast<Format>(value));
        }
        return table;
    }

    //==================================================================================================================
    FormatLayout formatLayout(Format format)
    {
        switch (format)
        {
        case Format::Undefined: return {};
        case Format::R8Unorm: return { 1, 1, 1 };
        case Format::R16Float: return { 1, 1, 2 };
        case Format::RGBA16Float: return { 1, 1, 8 };
        case Format::D32FloatS8: return {}; // No packed texel size: a copy layout never assumes one (chapter 15).
        case Format::BC1Unorm:
        case Format::BC1Unorm_sRGB: return { 4, 4, 8 };
        case Format::BGRA8Unorm:
        case Format::RGBA8Unorm:
        case Format::RGBA8Unorm_sRGB:
        case Format::RG16Float:
        case Format::D32Float:
        case Format::R32Float:
        case Format::RGB10A2Unorm: return { 1, 1, 4 };
        }
        return {};
    }

    //==================================================================================================================
    bool isBlockCompressed(Format format)
    {
        return classOf(format) == FormatClass::BlockCompressed;
    }

    //==================================================================================================================
    bool isDepthFormat(Format format)
    {
        return classOf(format) == FormatClass::Depth;
    }

    //==================================================================================================================
    bool samplesSupported(uint32_t samples, const FormatCapabilities& capabilities)
    {
        const bool valid = samples == 1 || samples == 2 || samples == 4 || samples == 8;
        return valid && ((capabilities.sampleCounts >> std::countr_zero(samples)) & 1) != 0;
    }

    //==================================================================================================================
    bool hasStencilPlane(Format format)
    {
        return format == Format::D32FloatS8;
    }

    //==================================================================================================================
    Format srgbCounterpart(Format format)
    {
        switch (format)
        {
        case Format::RGBA8Unorm: return Format::RGBA8Unorm_sRGB;
        case Format::RGBA8Unorm_sRGB: return Format::RGBA8Unorm;
        case Format::BC1Unorm: return Format::BC1Unorm_sRGB;
        case Format::BC1Unorm_sRGB: return Format::BC1Unorm;
        default: return Format::Undefined;
        }
    }

    //==================================================================================================================
    TileExtent standardTileExtent(Format format, TextureType type)
    {
        // D3D12 standard tile shapes, D3D12_TILED_RESOURCES_TIER standard swizzle, indexed by bits per texel (per block for a
        // block-compressed format): 8, 16, 32, 64, 128. Sparse textures are single-sample (VU-rojo-texture-0008), so the
        // multisampled shapes of the D3D12 table are not needed.
        constexpr TileExtent shapes2D[] = { { 256, 256, 1 }, { 256, 128, 1 }, { 128, 128, 1 }, { 128, 64, 1 }, { 64, 64, 1 } };
        constexpr TileExtent shapes3D[] = { { 64, 32, 32 }, { 32, 32, 32 }, { 32, 32, 16 }, { 32, 16, 16 }, { 16, 16, 16 } };
        const FormatLayout layout = formatLayout(format);
        // D32FloatS8 has no packed size; its two planes tile as a 64-bit texel, as D3D12 tiles D32_FLOAT_S8X24_UINT.
        const uint32_t blockBytes = layout.blockBytes != 0 ? layout.blockBytes : 8;
        ROJORHI_INTERNAL_ASSERT(blockBytes >= 1 && blockBytes <= 16 && std::has_single_bit(blockBytes), "a format has a block of an unsupported size");
        if (type == TextureType::Tex1D || type == TextureType::Tex1DArray)
        {
            return TileExtent{ 65536 / blockBytes * layout.blockWidth, 1, 1 };
        }
        const uint32_t index = static_cast<uint32_t>(std::min(std::countr_zero(blockBytes), 4));
        TileExtent extent = type == TextureType::Tex3D ? shapes3D[index] : shapes2D[index];
        extent.width *= layout.blockWidth;
        extent.height *= layout.blockHeight;
        return extent;
    }

    //==================================================================================================================
    void checkQueryFormat([[maybe_unused]] Format format)
    {
        ROJORHI_VU(static_cast<uint32_t>(format) < kFormatCount, "VU-rojo-format-0001", "{} is not a Format enumerator", static_cast<uint32_t>(format));
        ROJORHI_VU(format != Format::Undefined, "VU-rojo-format-0005", "formatCapabilities was called with Format::Undefined");
    }
} // namespace rojoRHI::core
