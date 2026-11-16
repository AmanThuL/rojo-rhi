//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4TextureDescriptor.cpp
/// @brief Implements the Metal 4 texture type and texture descriptor of a texture description.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4TextureDescriptor.h"

#include <rojoRHI/RHI.h>

#include "Core/Formats.h"
#include "Metal4Formats.h"

#include <Metal/Metal.hpp>

#include <cstdint>

namespace rojoRHI::metal4 {
    namespace {
        //==============================================================================================================
        /// Returns true when `usage` holds `flag`.
        bool hasUsage(TextureUsage usage, TextureUsage flag)
        {
            return (usage & flag) != TextureUsage::None;
        }

        //==============================================================================================================
        /// Returns the Metal usage of a texture of `format` with `usage`.
        MTL::TextureUsage textureUsage(Format format, TextureUsage usage)
        {
            MTL::TextureUsage native = MTL::TextureUsageUnknown;
            if (hasUsage(usage, TextureUsage::Sampled))
            {
                native |= MTL::TextureUsageShaderRead;
            }
            if (hasUsage(usage, TextureUsage::Storage))
            {
                native |= MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite;
            }
            if (hasUsage(usage, TextureUsage::RenderTarget) || hasUsage(usage, TextureUsage::DepthStencil))
            {
                native |= MTL::TextureUsageRenderTarget;
            }
            // Metal accepts a view of the sRGB or linear partner without the flag, so only the stencil plane of a
            // depth-stencil format, read through an X32_Stencil8 view of a Sampled texture, needs it. The flag costs
            // bandwidth where it is set, so no other texture carries it.
            if (core::hasStencilPlane(format) && hasUsage(usage, TextureUsage::Sampled))
            {
                native |= MTL::TextureUsagePixelFormatView;
            }
            return native;
        }
    } // namespace

    //==================================================================================================================
    MTL::TextureType textureType(TextureType type, uint32_t samples)
    {
        switch (type)
        {
        case TextureType::Tex1D: return MTL::TextureType1D;
        case TextureType::Tex1DArray: return MTL::TextureType1DArray;
        case TextureType::Tex2D: return samples > 1 ? MTL::TextureType2DMultisample : MTL::TextureType2D;
        case TextureType::Tex2DArray: return samples > 1 ? MTL::TextureType2DMultisampleArray : MTL::TextureType2DArray;
        case TextureType::Tex3D: return MTL::TextureType3D;
        case TextureType::Cube: return MTL::TextureTypeCube;
        case TextureType::CubeArray: return MTL::TextureTypeCubeArray;
        }
        return MTL::TextureType2D;
    }

    //==================================================================================================================
    MTL::TextureDescriptor* textureDescriptor(const TextureDesc& desc, bool placed)
    {
        MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
        descriptor->setTextureType(textureType(desc.type, desc.samples));
        descriptor->setPixelFormat(pixelFormat(desc.format));
        descriptor->setWidth(desc.extent.width);
        descriptor->setHeight(desc.extent.height);
        descriptor->setDepth(desc.extent.depth);
        descriptor->setMipmapLevelCount(desc.mips);
        uint32_t arrayLength = 1;
        switch (desc.type)
        {
        case TextureType::Tex1DArray:
        case TextureType::Tex2DArray: arrayLength = desc.layers; break;
        // RojoRHI counts faces, Metal counts cubes.
        case TextureType::Cube:
        case TextureType::CubeArray: arrayLength = desc.layers / 6; break;
        case TextureType::Tex1D:
        case TextureType::Tex2D:
        case TextureType::Tex3D: break;
        }
        descriptor->setArrayLength(arrayLength);
        descriptor->setSampleCount(desc.samples);
        descriptor->setStorageMode(MTL::StorageModePrivate);
        descriptor->setUsage(textureUsage(desc.format, desc.usage));
        if (placed)
        {
            descriptor->setHazardTrackingMode(MTL::HazardTrackingModeUntracked);
        }
        // A Sparse texture is a placement sparse texture whose tiles map onto 64 KiB pages of a heap.
        if (hasUsage(desc.usage, TextureUsage::Sparse))
        {
            descriptor->setPlacementSparsePageSize(MTL::SparsePageSize64);
        }
        return descriptor;
    }
} // namespace rojoRHI::metal4
