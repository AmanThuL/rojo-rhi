//----------------------------------------------------------------------------------------------------------------------
/// @file Formats.cpp
/// @brief Implements the per-class capability rules of chapter 15.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Formats.h"

#include "Base/Assert.h"

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
    void checkQueryFormat([[maybe_unused]] Format format)
    {
        ROJORHI_VU(static_cast<uint32_t>(format) < kFormatCount, "VU-rojo-format-0001", "{} is not a Format enumerator", static_cast<uint32_t>(format));
        ROJORHI_VU(format != Format::Undefined, "VU-rojo-format-0005", "formatCapabilities was called with Format::Undefined");
    }
} // namespace rojoRHI::core
