//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Formats.cpp
/// @brief Implements the Metal 4 pixel format of each `Format`, its inverse, and the format table a Metal 4 device reports.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Formats.h"

#include <rojoRHI/RHI.h>

#include "Core/Formats.h"

#include <Metal/Metal.hpp>

#include <cstdint>

namespace rojoRHI::metal4 {
    namespace {
        //==============================================================================================================
        FormatCapabilities& entry(core::FormatTable& table, Format format)
        {
            return table[static_cast<uint32_t>(format)];
        }
    } // namespace

    //==================================================================================================================
    MTL::PixelFormat pixelFormat(Format format)
    {
        switch (format)
        {
        case Format::Undefined: return MTL::PixelFormatInvalid;
        case Format::BGRA8Unorm: return MTL::PixelFormatBGRA8Unorm;
        case Format::RGBA8Unorm: return MTL::PixelFormatRGBA8Unorm;
        case Format::RGBA8Unorm_sRGB: return MTL::PixelFormatRGBA8Unorm_sRGB;
        case Format::RGBA16Float: return MTL::PixelFormatRGBA16Float;
        case Format::RG16Float: return MTL::PixelFormatRG16Float;
        case Format::R8Unorm: return MTL::PixelFormatR8Unorm;
        case Format::BC1Unorm: return MTL::PixelFormatBC1_RGBA;
        case Format::BC1Unorm_sRGB: return MTL::PixelFormatBC1_RGBA_sRGB;
        case Format::D32Float: return MTL::PixelFormatDepth32Float;
        case Format::R16Float: return MTL::PixelFormatR16Float;
        case Format::R32Float: return MTL::PixelFormatR32Float;
        case Format::D32FloatS8: return MTL::PixelFormatDepth32Float_Stencil8;
        case Format::RGB10A2Unorm: return MTL::PixelFormatRGB10A2Unorm;
        }
        return MTL::PixelFormatInvalid;
    }

    //==================================================================================================================
    Format formatOf(MTL::PixelFormat pixelFormat)
    {
        switch (pixelFormat)
        {
        case MTL::PixelFormatBGRA8Unorm: return Format::BGRA8Unorm;
        case MTL::PixelFormatRGBA8Unorm: return Format::RGBA8Unorm;
        case MTL::PixelFormatRGBA8Unorm_sRGB: return Format::RGBA8Unorm_sRGB;
        case MTL::PixelFormatRGBA16Float: return Format::RGBA16Float;
        case MTL::PixelFormatRG16Float: return Format::RG16Float;
        case MTL::PixelFormatR8Unorm: return Format::R8Unorm;
        case MTL::PixelFormatBC1_RGBA: return Format::BC1Unorm;
        case MTL::PixelFormatBC1_RGBA_sRGB: return Format::BC1Unorm_sRGB;
        case MTL::PixelFormatDepth32Float: return Format::D32Float;
        case MTL::PixelFormatR16Float: return Format::R16Float;
        case MTL::PixelFormatR32Float: return Format::R32Float;
        case MTL::PixelFormatDepth32Float_Stencil8: return Format::D32FloatS8;
        case MTL::PixelFormatRGB10A2Unorm: return Format::RGB10A2Unorm;
        default: return Format::Undefined;
        }
    }

    //==================================================================================================================
    core::FormatTable metalFormatTable(MTL::Device* device)
    {
        core::FormatTable table = core::formatCeilings();
        for (FormatCapabilities& capabilities : table)
        {
            // Sparse resources are not created yet, so the device reports placementSparse false and no format reports sparse.
            capabilities.sparse = false;
            // Each count follows supportsTextureSampleCount; bit i of the mask is a count of 1 << i, so count n is bit mask n.
            for (const uint32_t samples : { 2u, 4u, 8u })
            {
                if (!device->supportsTextureSampleCount(samples))
                {
                    capabilities.sampleCounts &= ~samples;
                }
            }
        }
        // BC formats need supportsBCTextureCompression (chapter 15), which checkFloor requires, so they keep their ceiling.
        // R32Float: the Apple7 and Apple8 columns list Write, Color, MSAA and Blend, and Apple9 and later list All; footnote 6
        // gives Filter and Resolve to the devices that report supports32BitFloatFiltering (Metal Feature Set Tables, May 2026,
        // ordinary 32-bit pixel formats). Blend stays at the ceiling.
        entry(table, Format::R32Float).filterLinear = device->supports32BitFloatFiltering();
        entry(table, Format::R32Float).resolve = device->supports32BitFloatFiltering();
        // A Storage view is read and written: the shader ABI's rwTexture accessors are read_write, and read-write texture tier 2
        // lists neither BGRA8Unorm, RG16Float nor RGB10A2Unorm, so a shader can neither read nor write them through one.
        entry(table, Format::BGRA8Unorm).storageRead = false;
        entry(table, Format::BGRA8Unorm).storageWrite = false;
        entry(table, Format::RGB10A2Unorm).storageRead = false;
        entry(table, Format::RGB10A2Unorm).storageWrite = false;
        // Chapter 15 guarantees RG16Float storage writes at the floor, and the Apple family tables list Write for it, so it keeps
        // storageWrite; a shader writes it through a write-only accessor, which the shader ABI does not provide yet.
        entry(table, Format::RG16Float).storageRead = false;
        // Depth32Float and Depth32Float_Stencil8: the Apple7 and Apple8 columns list MSAA and Resolve, and Apple9 and later add
        // Filter (Metal Feature Set Tables, May 2026, depth and stencil pixel formats).
        const bool depthFilter = device->supportsFamily(MTL::GPUFamilyApple9);
        entry(table, Format::D32Float).filterLinear = depthFilter;
        entry(table, Format::D32FloatS8).filterLinear = depthFilter;
        return table;
    }
} // namespace rojoRHI::metal4
