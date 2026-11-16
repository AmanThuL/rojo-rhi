//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Copies.cpp
/// @brief Implements the D3D12 copy entries: one native copy for a buffer range and one per layer for a texture region.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Device.h"

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/Copies.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/Formats.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "D3D12Addr.h"
#include "D3D12Common.h"
#include "D3D12ListState.h"
#include "D3D12Textures.h"

#include <cstdint>

namespace rojoRHI {
    namespace {
        /// How a copy between a buffer and a texture walks the buffer: the footprint of one image and the distance between images.
        struct BufferImages
        {
            uint32_t width = 0;      ///< The footprint's width in texels, whole blocks.
            uint32_t height = 0;     ///< The footprint's height in texels, whole blocks: `rowsPerImage` rows when it is set.
            uint32_t boxHeight = 0;  ///< The region's height in texels, whole blocks.
            uint64_t imageBytes = 0; ///< Bytes between consecutive images in the buffer.
        };

        //==============================================================================================================
        /// Returns `value` rounded up to a multiple of `edge`.
        uint32_t roundUp(uint32_t value, uint32_t edge)
        {
            return (value + edge - 1) / edge * edge;
        }

        //==============================================================================================================
        /// Returns how a buffer laid out by `layout` holds the images of `region` of a texture of `format`.
        BufferImages bufferImages(Format format, const BufferTextureLayout& layout, const TextureRegion& region)
        {
            const core::FormatLayout packing = core::formatLayout(format);
            BufferImages images;
            images.width = roundUp(region.extent.width, packing.blockWidth);
            images.boxHeight = roundUp(region.extent.height, packing.blockHeight);
            images.height = layout.rowsPerImage != 0 ? layout.rowsPerImage * packing.blockHeight : images.boxHeight;
            images.imageBytes = uint64_t{ layout.rowPitch } * (images.height / packing.blockHeight);
            return images;
        }

        //==============================================================================================================
        /// Returns the location of subresource `subresource` of `resource`.
        D3D12_TEXTURE_COPY_LOCATION subresourceLocation(ID3D12Resource* resource, uint32_t subresource)
        {
            D3D12_TEXTURE_COPY_LOCATION location = {};
            location.pResource = resource;
            location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            location.SubresourceIndex = subresource;
            return location;
        }

        //==============================================================================================================
        /// Returns the location of a `depth`-slice image of `images` in `resource` at byte `offset`, its rows `rowPitch` apart.
        D3D12_TEXTURE_COPY_LOCATION footprintLocation(
            ID3D12Resource* resource, uint64_t offset, DXGI_FORMAT format, const BufferImages& images, uint32_t depth, uint32_t rowPitch
        )
        {
            D3D12_TEXTURE_COPY_LOCATION location = {};
            location.pResource = resource;
            location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            location.PlacedFootprint.Offset = offset;
            location.PlacedFootprint.Footprint.Format = format;
            location.PlacedFootprint.Footprint.Width = images.width;
            location.PlacedFootprint.Footprint.Height = images.height;
            location.PlacedFootprint.Footprint.Depth = depth;
            location.PlacedFootprint.Footprint.RowPitch = rowPitch;
            return location;
        }

        //==============================================================================================================
        /// Returns the box of `region`'s texels in a texture, `width` by `height` texels: the region's depth slices when `volume`
        /// is set, else one slice.
        D3D12_BOX boxOf(const TextureRegion& region, uint32_t width, uint32_t height, bool volume)
        {
            D3D12_BOX box = {};
            box.left = region.offset.x;
            box.top = region.offset.y;
            box.front = volume ? region.offset.z : 0;
            box.right = region.offset.x + width;
            box.bottom = region.offset.y + height;
            box.back = box.front + (volume ? region.extent.depth : 1);
            return box;
        }

        //==============================================================================================================
        /// Returns the native texture a copy of `list` names; the payload is fixed from creation to retirement, so it is read with
        /// one load and no lock (ADR 0020).
        const d3d12::TexturePayload& texturePayload(const core::CommandListCore& list, Texture texture)
        {
            return *static_cast<const d3d12::TexturePayload*>(list.device->pool<Texture>().native(texture));
        }

        //==============================================================================================================
        /// Returns the native buffer a copy of `list` names, read as `texturePayload` reads a texture.
        ID3D12Resource* nativeBuffer(const core::CommandListCore& list, Buffer buffer)
        {
            return static_cast<const d3d12::BufferPayload*>(list.device->pool<Buffer>().native(buffer))->resource;
        }
    } // namespace

    //==================================================================================================================
    void CommandList::copyBuffer(Buffer src, uint64_t srcOffset, Buffer dst, uint64_t dstOffset, uint64_t size)
    {
        CommandListImpl& impl = CommandListImpl::of(*this);
        core::CommandListCore& list = impl.core;
        core::copyBuffer(list, src, srcOffset, dst, dstOffset, size);
        impl.state.list->CopyBufferRegion(nativeBuffer(list, dst), dstOffset, nativeBuffer(list, src), srcOffset, size);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::copyBufferToTexture(Buffer src, const BufferTextureLayout& layout, Texture dst, const TextureRegion& region)
    {
        CommandListImpl& impl = CommandListImpl::of(*this);
        core::CommandListCore& list = impl.core;
        core::copyBufferToTexture(list, src, layout, dst, region);
        ID3D12Resource* buffer = nativeBuffer(list, src);
        const d3d12::TexturePayload& texture = texturePayload(list, dst);
        const BufferImages images = bufferImages(texture.format, layout, region);
        const bool volume = texture.type == TextureType::Tex3D;
        // The box is in the source, the buffer's footprint, whose origin is its first texel; the texture's offset is the target.
        const D3D12_BOX box = boxOf(TextureRegion{ .extent = region.extent }, images.width, images.boxHeight, volume);
        const uint32_t calls = volume ? 1 : region.layers;
        for (uint32_t layer = 0; layer < calls; ++layer)
        {
            const D3D12_TEXTURE_COPY_LOCATION target = subresourceLocation(texture.resource, region.mip + (region.layer + layer) * texture.mips);
            const D3D12_TEXTURE_COPY_LOCATION source = footprintLocation(
                buffer, layout.offset + layer * images.imageBytes, texture.copyFormat, images, volume ? region.extent.depth : 1, layout.rowPitch
            );
            impl.state.list->CopyTextureRegion(&target, region.offset.x, region.offset.y, volume ? region.offset.z : 0, &source, &box);
            core::noteNativeCall(list);
        }
    }

    //==================================================================================================================
    void CommandList::copyTextureToBuffer(Texture src, const TextureRegion& region, Buffer dst, const BufferTextureLayout& layout)
    {
        CommandListImpl& impl = CommandListImpl::of(*this);
        core::CommandListCore& list = impl.core;
        core::copyTextureToBuffer(list, src, region, dst, layout);
        ID3D12Resource* buffer = nativeBuffer(list, dst);
        const d3d12::TexturePayload& texture = texturePayload(list, src);
        const BufferImages images = bufferImages(texture.format, layout, region);
        const bool volume = texture.type == TextureType::Tex3D;
        const D3D12_BOX box = boxOf(region, images.width, images.boxHeight, volume);
        const uint32_t calls = volume ? 1 : region.layers;
        for (uint32_t layer = 0; layer < calls; ++layer)
        {
            const D3D12_TEXTURE_COPY_LOCATION source = subresourceLocation(texture.resource, region.mip + (region.layer + layer) * texture.mips);
            const D3D12_TEXTURE_COPY_LOCATION target = footprintLocation(
                buffer, layout.offset + layer * images.imageBytes, texture.copyFormat, images, volume ? region.extent.depth : 1, layout.rowPitch
            );
            impl.state.list->CopyTextureRegion(&target, 0, 0, 0, &source, &box);
            core::noteNativeCall(list);
        }
    }

    //==================================================================================================================
    void CommandList::copyTexture(Texture src, const TextureRegion& srcRegion, Texture dst, const TextureRegion& dstRegion)
    {
        CommandListImpl& impl = CommandListImpl::of(*this);
        core::CommandListCore& list = impl.core;
        core::copyTexture(list, src, srcRegion, dst, dstRegion);
        const d3d12::TexturePayload& source = texturePayload(list, src);
        const d3d12::TexturePayload& target = texturePayload(list, dst);
        const bool sourceVolume = source.type == TextureType::Tex3D;
        const bool destinationVolume = target.type == TextureType::Tex3D;
        // The regions have equal extents and layer counts, so a copy that names a 3D texture is one call over the depth.
        const bool volume = sourceVolume || destinationVolume;
        const uint32_t calls = volume ? 1 : srcRegion.layers;
        // The regions have equal extents in blocks, not in texels: a region reaching a block-compressed mip's edge may end in a
        // partial block, and the native box counts whole blocks.
        const core::FormatLayout packing = core::formatLayout(source.format);
        const D3D12_BOX box =
            boxOf(srcRegion, roundUp(srcRegion.extent.width, packing.blockWidth), roundUp(srcRegion.extent.height, packing.blockHeight), sourceVolume);
        for (uint32_t layer = 0; layer < calls; ++layer)
        {
            const D3D12_TEXTURE_COPY_LOCATION from = subresourceLocation(source.resource, srcRegion.mip + (srcRegion.layer + layer) * source.mips);
            const D3D12_TEXTURE_COPY_LOCATION to = subresourceLocation(target.resource, dstRegion.mip + (dstRegion.layer + layer) * target.mips);
            impl.state.list->CopyTextureRegion(&to, dstRegion.offset.x, dstRegion.offset.y, destinationVolume ? dstRegion.offset.z : 0, &from, &box);
            core::noteNativeCall(list);
        }
    }
} // namespace rojoRHI
