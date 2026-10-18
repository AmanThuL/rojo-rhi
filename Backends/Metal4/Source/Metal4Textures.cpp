//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Textures.cpp
/// @brief Implements the texture entries of the Metal 4 backend and their retire hook.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Textures.h"

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Heaps.h"
#include "Core/Records.h"
#include "Core/Textures.h"
#include "Metal4Common.h"
#include "Metal4Device.h"
#include "Metal4Formats.h"
#include "Metal4Sparse.h"
#include "Metal4TextureDescriptor.h"

#include <Metal/Metal.hpp>

#include <format>
#include <span>
#include <utility>

namespace rojoRHI {
    namespace {
        //==============================================================================================================
        /// Returns the bytes and alignment a texture of `desc` occupies in a heap on `device`. The caller holds an autorelease
        /// pool.
        PlacementRequirements placedRequirements(MTL::Device* device, const TextureDesc& desc)
        {
            MTL::TextureDescriptor* descriptor = metal4::textureDescriptor(desc, true);
            const MTL::SizeAndAlign answer = device->heapTextureSizeAndAlign(descriptor);
            descriptor->release();
            return PlacementRequirements{ answer.size, answer.align };
        }

        //==============================================================================================================
        /// Undoes a texture creation that failed after `texture` was made: it leaves the device residency set unless it is
        /// placed, and is released. The caller holds an autorelease pool.
        void discardTexture(DeviceImpl& impl, MTL::Texture* texture, bool placed)
        {
            if (!placed)
            {
                impl.residency.remove(texture);
            }
            texture->release();
        }

        //==============================================================================================================
        /// Returns the name of `type` as `TextureType` spells it.
        const char* typeName(TextureType type)
        {
            switch (type)
            {
            case TextureType::Tex1D: return "Tex1D";
            case TextureType::Tex1DArray: return "Tex1DArray";
            case TextureType::Tex2D: return "Tex2D";
            case TextureType::Tex2DArray: return "Tex2DArray";
            case TextureType::Tex3D: return "Tex3D";
            case TextureType::Cube: return "Cube";
            case TextureType::CubeArray: return "CubeArray";
            }
            return "unknown";
        }
    } // namespace

    namespace metal4 {
        //==============================================================================================================
        void retireTexture(void* context, core::TextureRecord& record)
        {
            auto* texture = static_cast<MTL::Texture*>(record.native);
            if (texture == nullptr)
            {
                return;
            }
            DeviceImpl& impl = *static_cast<DeviceImpl*>(context);
            const AutoreleasePool pool;
            // A placed texture never joined the set: its heap did. A swapchain image is a drawable's texture, which the layer's
            // set holds; its record holds one reference.
            if (record.heap.bits == 0 && !record.swapchainImage)
            {
                impl.residency.remove(texture);
            }
            texture->release();
            record.native = nullptr;
        }
    } // namespace metal4

    //==================================================================================================================
    PlacementRequirements Device::placementRequirements(const TextureDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::checkTextureRequirementsQuery(impl.core, desc);
        const metal4::AutoreleasePool pool;
        return placedRequirements(impl.device, desc);
    }

    //==================================================================================================================
    Result<Texture> Device::createTexture(const TextureDesc& desc, std::span<const TextureData> initial, Placement placement)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        const bool placed = placement.heap.bits != 0;
        const metal4::AutoreleasePool pool;
        PlacementRequirements requirements;
        if (placed)
        {
            // Metal never sees a descriptor the core rejects: a size query of an invalid texture could abort inside Metal
            // before the violation is reported.
            core::checkTextureDesc(impl.core, desc);
            requirements = placedRequirements(impl.device, desc);
        }
        core::TextureRecord record = core::prepareTexture(impl.core, desc, initial, placement, requirements);
        const bool sparse = (desc.usage & TextureUsage::Sparse) != TextureUsage::None;
        // A type Metal does not tile has no tile shape, so it is refused before a native texture exists.
        if (sparse && metal4::sparseTileExtent(impl.device, metal4::textureType(desc.type, desc.samples), metal4::pixelFormat(desc.format)).width == 0)
        {
            return std::unexpected(
                Error{ ErrorCategory::Unsupported, std::format("the device has no sparse tile for a {} texture, \"{}\"", typeName(desc.type), record.label) }
            );
        }
        MTL::TextureDescriptor* descriptor = metal4::textureDescriptor(desc, placed);
        MTL::Texture* texture = nullptr;
        if (placed)
        {
            auto* heap = static_cast<MTL::Heap*>(core::checkedHeap(impl.core, placement.heap).native);
            texture = heap->newTexture(descriptor, placement.offset);
        }
        else
        {
            texture = impl.device->newTexture(descriptor);
        }
        descriptor->release();
        if (texture == nullptr)
        {
            return std::unexpected(Error{ ErrorCategory::OutOfMemory, std::format("no device memory for texture \"{}\"", record.label) });
        }
        texture->setLabel(metal4::makeString(record.label));
        if (sparse)
        {
            // Set before the handle exists: the core reads the shape without the pool's lock.
            record.tiles = metal4::measureTiles(impl.device, texture, desc.mips);
        }
        if (!placed)
        {
            impl.residency.add(texture);
        }
        if (!initial.empty())
        {
            if (Result<void> uploaded = impl.stager.uploadTexture(impl, record, initial, texture); !uploaded)
            {
                discardTexture(impl, texture, placed);
                return std::unexpected(std::move(uploaded.error()));
            }
        }
        record.native = texture;
        Result<Texture> handle = core::addTexture(impl.core, std::move(record));
        if (!handle)
        {
            discardTexture(impl, texture, placed);
        }
        return handle;
    }

    //==================================================================================================================
    void Device::destroy(Texture texture)
    {
        core::destroyTexture(DeviceImpl::of(*this).core, texture);
    }
} // namespace rojoRHI
