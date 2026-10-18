//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Copies.cpp
/// @brief Implements the copy and fill entries for the Metal 4 backend: each records on the list's lazily opened compute encoder.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Device.h"

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/Copies.h"
#include "Core/DeviceCore.h"
#include "Core/Formats.h"
#include "Core/HandlePool.h"
#include "Metal4Common.h"
#include "Metal4Encoders.h"
#include "Metal4Formats.h"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <cstdint>

namespace rojoRHI {
    namespace {
        //==============================================================================================================
        /// The native buffer `buffer` names. A record's payload is fixed from creation to retirement, so it is read with one
        /// load and no lock (ADR 0020).
        const MTL::Buffer* nativeBuffer(const core::CommandListCore& list, Buffer buffer)
        {
            return static_cast<const MTL::Buffer*>(list.device->pool<Buffer>().native(buffer));
        }

        //==============================================================================================================
        /// The native texture `texture` names, read as `nativeBuffer` reads a buffer.
        const MTL::Texture* nativeTexture(const core::CommandListCore& list, Texture texture)
        {
            return static_cast<const MTL::Texture*>(list.device->pool<Texture>().native(texture));
        }

        /// How a copy between a buffer and a texture walks the buffer: one native call for a volume, else one per layer.
        struct BufferImages
        {
            bool volume = false;        ///< The texture is 3D: one call covers every slice of the region.
            uint64_t imageBytes = 0;    ///< Bytes between consecutive images in the buffer.
            uint64_t bytesPerImage = 0; ///< The native call's bytes-per-image argument: `imageBytes` for 3D and array types, else 0.
        };

        //==============================================================================================================
        /// Returns how a copy of `region` of `texture` walks a buffer laid out by `layout`. The block height comes from the
        /// texture's own pixel format, read through its getter rather than from the record, which is behind a lock.
        BufferImages bufferImages(const MTL::Texture* texture, const BufferTextureLayout& layout, const TextureRegion& region)
        {
            const core::FormatLayout packing = core::formatLayout(metal4::formatOf(texture->pixelFormat()));
            const uint64_t heightInBlocks = (uint64_t{ region.extent.height } + packing.blockHeight - 1) / packing.blockHeight;
            const uint64_t rows = layout.rowsPerImage != 0 ? layout.rowsPerImage : heightInBlocks;
            BufferImages images;
            images.imageBytes = uint64_t{ layout.rowPitch } * rows;
            switch (texture->textureType())
            {
            case MTL::TextureType3D:
                images.volume = true;
                images.bytesPerImage = images.imageBytes;
                break;
            case MTL::TextureType1DArray:
            case MTL::TextureType2DArray:
            case MTL::TextureTypeCube:
            case MTL::TextureTypeCubeArray: images.bytesPerImage = images.imageBytes; break;
            default: break;
            }
            return images;
        }
    } // namespace

    //==================================================================================================================
    void CommandList::copyBuffer(Buffer src, uint64_t srcOffset, Buffer dst, uint64_t dstOffset, uint64_t size)
    {
        CommandListImpl& impl = CommandListImpl::of(*this);
        core::CommandListCore& list = impl.core;
        core::copyBuffer(list, src, srcOffset, dst, dstOffset, size);
        const MTL::Buffer* source = nativeBuffer(list, src);
        const MTL::Buffer* destination = nativeBuffer(list, dst);
        MTL4::ComputeCommandEncoder* encoder = impl.encoders.compute(list, impl.state);
        // The driver builds an internal blit pipeline on a shape's first use and autoreleases while it does.
        const metal4::AutoreleasePool pool;
        encoder->copyFromBuffer(source, srcOffset, destination, dstOffset, size);
        core::noteNativeCall(list);
        impl.encoders.markWork();
    }

    //==================================================================================================================
    void CommandList::copyBufferToTexture(Buffer src, const BufferTextureLayout& layout, Texture dst, const TextureRegion& region)
    {
        CommandListImpl& impl = CommandListImpl::of(*this);
        core::CommandListCore& list = impl.core;
        core::copyBufferToTexture(list, src, layout, dst, region);
        const MTL::Buffer* source = nativeBuffer(list, src);
        const MTL::Texture* texture = nativeTexture(list, dst);
        const BufferImages images = bufferImages(texture, layout, region);
        MTL4::ComputeCommandEncoder* encoder = impl.encoders.compute(list, impl.state);
        // The driver builds an internal blit pipeline on a shape's first use and autoreleases while it does.
        const metal4::AutoreleasePool pool;
        if (images.volume)
        {
            encoder->copyFromBuffer(
                source,
                layout.offset,
                layout.rowPitch,
                images.bytesPerImage,
                MTL::Size(region.extent.width, region.extent.height, region.extent.depth),
                texture,
                0,
                region.mip,
                MTL::Origin(region.offset.x, region.offset.y, region.offset.z)
            );
            core::noteNativeCall(list);
        }
        else
        {
            // One call per layer, a cube face counting as a slice.
            for (uint32_t layer = 0; layer < region.layers; ++layer)
            {
                encoder->copyFromBuffer(
                    source,
                    layout.offset + layer * images.imageBytes,
                    layout.rowPitch,
                    images.bytesPerImage,
                    MTL::Size(region.extent.width, region.extent.height, 1),
                    texture,
                    region.layer + layer,
                    region.mip,
                    MTL::Origin(region.offset.x, region.offset.y, 0)
                );
                core::noteNativeCall(list);
            }
        }
        impl.encoders.markWork();
    }

    //==================================================================================================================
    void CommandList::copyTextureToBuffer(Texture src, const TextureRegion& region, Buffer dst, const BufferTextureLayout& layout)
    {
        CommandListImpl& impl = CommandListImpl::of(*this);
        core::CommandListCore& list = impl.core;
        core::copyTextureToBuffer(list, src, region, dst, layout);
        const MTL::Texture* texture = nativeTexture(list, src);
        const MTL::Buffer* destination = nativeBuffer(list, dst);
        const BufferImages images = bufferImages(texture, layout, region);
        MTL4::ComputeCommandEncoder* encoder = impl.encoders.compute(list, impl.state);
        // The driver builds an internal blit pipeline on a shape's first use and autoreleases while it does.
        const metal4::AutoreleasePool pool;
        if (images.volume)
        {
            encoder->copyFromTexture(
                texture,
                0,
                region.mip,
                MTL::Origin(region.offset.x, region.offset.y, region.offset.z),
                MTL::Size(region.extent.width, region.extent.height, region.extent.depth),
                destination,
                layout.offset,
                layout.rowPitch,
                images.bytesPerImage
            );
            core::noteNativeCall(list);
        }
        else
        {
            for (uint32_t layer = 0; layer < region.layers; ++layer)
            {
                encoder->copyFromTexture(
                    texture,
                    region.layer + layer,
                    region.mip,
                    MTL::Origin(region.offset.x, region.offset.y, 0),
                    MTL::Size(region.extent.width, region.extent.height, 1),
                    destination,
                    layout.offset + layer * images.imageBytes,
                    layout.rowPitch,
                    images.bytesPerImage
                );
                core::noteNativeCall(list);
            }
        }
        impl.encoders.markWork();
    }

    //==================================================================================================================
    void CommandList::copyTexture(Texture src, const TextureRegion& srcRegion, Texture dst, const TextureRegion& dstRegion)
    {
        CommandListImpl& impl = CommandListImpl::of(*this);
        core::CommandListCore& list = impl.core;
        core::copyTexture(list, src, srcRegion, dst, dstRegion);
        const MTL::Texture* source = nativeTexture(list, src);
        const MTL::Texture* destination = nativeTexture(list, dst);
        // A 3D texture is addressed by its depth origin at slice 0; any other texture by its slices, one call per layer. The
        // regions have equal extents and layer counts, so a copy that names a 3D texture is one call over the depth.
        const bool sourceVolume = source->textureType() == MTL::TextureType3D;
        const bool destinationVolume = destination->textureType() == MTL::TextureType3D;
        const bool volume = sourceVolume || destinationVolume;
        const uint32_t calls = volume ? 1 : srcRegion.layers;
        // The regions have equal extents in blocks, not in texels: a region reaching a block-compressed mip's edge may end in a
        // partial block. The native size counts whole blocks, which Metal accepts and clamps at the mip's edge on either side; a
        // size ending inside a block is refused when the destination region lies inside its mip.
        const core::FormatLayout packing = core::formatLayout(metal4::formatOf(source->pixelFormat()));
        const auto blocks = [](uint32_t texels, uint32_t edge) { return (texels + edge - 1) / edge * edge; };
        const MTL::Size size(
            blocks(srcRegion.extent.width, packing.blockWidth), blocks(srcRegion.extent.height, packing.blockHeight), volume ? srcRegion.extent.depth : 1
        );
        MTL4::ComputeCommandEncoder* encoder = impl.encoders.compute(list, impl.state);
        // The driver builds an internal blit pipeline on a shape's first use and autoreleases while it does.
        const metal4::AutoreleasePool pool;
        for (uint32_t layer = 0; layer < calls; ++layer)
        {
            // Formats that differ only in sRGB transfer copy directly: the stored bits move unchanged.
            encoder->copyFromTexture(
                source,
                sourceVolume ? 0 : srcRegion.layer + layer,
                srcRegion.mip,
                MTL::Origin(srcRegion.offset.x, srcRegion.offset.y, sourceVolume ? srcRegion.offset.z : 0),
                size,
                destination,
                destinationVolume ? 0 : dstRegion.layer + layer,
                dstRegion.mip,
                MTL::Origin(dstRegion.offset.x, dstRegion.offset.y, destinationVolume ? dstRegion.offset.z : 0)
            );
            core::noteNativeCall(list);
        }
        impl.encoders.markWork();
    }

    //==================================================================================================================
    void CommandList::fillBuffer(Buffer dst, uint64_t offset, uint64_t size, uint32_t pattern)
    {
        CommandListImpl& impl = CommandListImpl::of(*this);
        core::CommandListCore& list = impl.core;
        core::fillBuffer(list, dst, offset, size);
        const auto byte = static_cast<uint8_t>(pattern & 0xFFu);
        if (pattern != byte * 0x01010101u)
        {
            metal4::notImplemented("fillBuffer with a pattern of unequal bytes");
        }
        const MTL::Buffer* destination = nativeBuffer(list, dst);
        MTL4::ComputeCommandEncoder* encoder = impl.encoders.compute(list, impl.state);
        // The driver builds an internal blit pipeline on a shape's first use and autoreleases while it does.
        const metal4::AutoreleasePool pool;
        encoder->fillBuffer(destination, NS::Range(offset, size), byte);
        core::noteNativeCall(list);
        impl.encoders.markWork();
    }
} // namespace rojoRHI
