//----------------------------------------------------------------------------------------------------------------------
/// @file Copies.cpp
/// @brief Implements the checks of the copy, fill and clear rules.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Copies.h"

#include "Base/Assert.h"
#include "Core/Buffers.h"
#include "Core/Formats.h"
#include "Core/Textures.h"

#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>

namespace rojoRHI::core {
    namespace {
        //==============================================================================================================
        [[maybe_unused]] bool hasUsage(BufferUsage usage, BufferUsage flag)
        {
            return (usage & flag) != BufferUsage::None;
        }

        //==============================================================================================================
        [[maybe_unused]] bool hasUsage(TextureUsage usage, TextureUsage flag)
        {
            return (usage & flag) != TextureUsage::None;
        }

        //==============================================================================================================
        /// Returns `a + b`, or nothing when the sum wraps.
        [[maybe_unused]] std::optional<uint64_t> checkedAdd(uint64_t a, uint64_t b)
        {
            if (b > std::numeric_limits<uint64_t>::max() - a)
            {
                return std::nullopt;
            }
            return a + b;
        }

        //==============================================================================================================
        /// Returns `a * b`, or nothing when the product wraps.
        [[maybe_unused]] std::optional<uint64_t> checkedMul(uint64_t a, uint64_t b)
        {
            if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a)
            {
                return std::nullopt;
            }
            return a * b;
        }

        //==============================================================================================================
        /// Returns true when `size` bytes from `offset` lie inside a buffer of `bufferSize` bytes; a wrapping end is outside.
        [[maybe_unused]] bool rangeInside(uint64_t offset, uint64_t size, uint64_t bufferSize)
        {
            return size <= bufferSize && offset <= bufferSize - size;
        }

        //==============================================================================================================
        [[maybe_unused]] uint64_t divCeil(uint64_t value, uint64_t divisor)
        {
            return (value + divisor - 1) / divisor;
        }

        //==============================================================================================================
        /// Returns the half-open end of the bytes `layout` addresses for `region` of a texture of `format`, or nothing when the
        /// end wraps: the first block at `offset`, rows `rowPitch` apart, images `rowPitch * rowsPerImage` apart.
        [[maybe_unused]] std::optional<uint64_t> layoutEnd(Format format, const BufferTextureLayout& layout, const TextureRegion& region)
        {
            const FormatLayout block = formatLayout(format);
            const uint64_t widthBlocks = divCeil(region.extent.width, block.blockWidth);
            const uint64_t heightBlocks = divCeil(region.extent.height, block.blockHeight);
            const uint64_t rowsPerImage = layout.rowsPerImage != 0 ? layout.rowsPerImage : heightBlocks;
            const uint64_t images = uint64_t{ region.extent.depth } * region.layers;
            const uint64_t imageBytes = uint64_t{ layout.rowPitch } * rowsPerImage;
            const uint64_t lastRowStart = (heightBlocks - 1) * layout.rowPitch;
            const std::optional<uint64_t> lastImageStart = checkedMul(images - 1, imageBytes);
            if (!lastImageStart)
            {
                return std::nullopt;
            }
            const std::optional<uint64_t> lastRow = checkedAdd(*lastImageStart, lastRowStart);
            const std::optional<uint64_t> lastBlock = lastRow ? checkedAdd(*lastRow, widthBlocks * block.blockBytes) : std::nullopt;
            return lastBlock ? checkedAdd(*lastBlock, layout.offset) : std::nullopt;
        }

        //==============================================================================================================
        /// Says what keeps `list` from fill or clear (outside a pass, on a Graphics or Compute list), or returns an empty
        /// string.
        [[maybe_unused]] std::string computeCopyProblem(const CommandListCore& list)
        {
            if (list.passOpen)
            {
                return "a render pass is open";
            }
            if (list.type == QueueType::Copy)
            {
                return "the list is a Copy list";
            }
            return {};
        }

        //==============================================================================================================
        /// Says why `size` bytes from `offset` are not a non-empty, 4-byte aligned range of a buffer of `bufferSize` bytes, or
        /// returns an empty string.
        [[maybe_unused]] std::string bufferRangeProblem(uint64_t offset, uint64_t size, uint64_t bufferSize)
        {
            if (size == 0)
            {
                return "the size is zero";
            }
            if (offset % 4 != 0 || size % 4 != 0)
            {
                return std::format("the offset {} and size {} are not both multiples of 4", offset, size);
            }
            if (!rangeInside(offset, size, bufferSize))
            {
                return std::format("{} bytes from offset {} leave the buffer of {} bytes", size, offset, bufferSize);
            }
            return {};
        }

        //==============================================================================================================
        /// Checks that `region` is a box of `texture` a copy may name (VU-rojo-cmd-0038 to 0041); the one site of the rules,
        /// shared by every copy that names a texture.
        [[maybe_unused]] void checkTextureRegion([[maybe_unused]] const TextureRecord& texture, [[maybe_unused]] const TextureRegion& region)
        {
            ROJORHI_VU(
                region.mip < texture.mips && region.layers >= 1 && uint64_t{ region.layer } + region.layers <= texture.layers,
                "VU-rojo-cmd-0038",
                "the region names mip {} and layers {} to {}, outside the texture \"{}\" of {} mips and {} layers",
                region.mip,
                region.layer,
                uint64_t{ region.layer } + region.layers,
                texture.label,
                texture.mips,
                texture.layers
            );
            [[maybe_unused]] const Extent3D mip = mipExtent(texture.extent, region.mip);
            ROJORHI_VU(
                region.extent.width > 0 && region.extent.height > 0 && region.extent.depth > 0 &&
                    uint64_t{ region.offset.x } + region.extent.width <= mip.width && uint64_t{ region.offset.y } + region.extent.height <= mip.height &&
                    uint64_t{ region.offset.z } + region.extent.depth <= mip.depth,
                "VU-rojo-cmd-0039",
                "the region of {} by {} by {} texels at ({}, {}, {}) is empty or leaves mip {} of {} by {} by {} texels",
                region.extent.width,
                region.extent.height,
                region.extent.depth,
                region.offset.x,
                region.offset.y,
                region.offset.z,
                region.mip,
                mip.width,
                mip.height,
                mip.depth
            );
            [[maybe_unused]] const FormatLayout block = formatLayout(texture.format);
            ROJORHI_VU(
                !isBlockCompressed(texture.format) ||
                    (region.offset.x % block.blockWidth == 0 && region.offset.y % block.blockHeight == 0 &&
                     (region.extent.width % block.blockWidth == 0 || uint64_t{ region.offset.x } + region.extent.width == mip.width) &&
                     (region.extent.height % block.blockHeight == 0 || uint64_t{ region.offset.y } + region.extent.height == mip.height)),
                "VU-rojo-cmd-0040",
                "the region at ({}, {}) of {} by {} texels does not follow the {} by {} blocks of the texture \"{}\"",
                region.offset.x,
                region.offset.y,
                region.extent.width,
                region.extent.height,
                block.blockWidth,
                block.blockHeight,
                texture.label
            );
            ROJORHI_VU(
                !hasStencilPlane(texture.format), "VU-rojo-cmd-0041", "the texture \"{}\" has a stencil component, which copies do not support", texture.label
            );
        }

        //==============================================================================================================
        /// Checks `layout` against `region` of `texture` (VU-rojo-cmd-0034 to 0037); the one site of the rules.
        [[maybe_unused]] void checkLayout(
            [[maybe_unused]] const TextureRecord& texture, [[maybe_unused]] const BufferTextureLayout& layout, [[maybe_unused]] const TextureRegion& region
        )
        {
            [[maybe_unused]] const FormatLayout block = formatLayout(texture.format);
            [[maybe_unused]] const uint64_t rowBytes = divCeil(region.extent.width, block.blockWidth) * block.blockBytes;
            [[maybe_unused]] const uint64_t heightBlocks = divCeil(region.extent.height, block.blockHeight);
            ROJORHI_VU(layout.offset % 512 == 0, "VU-rojo-cmd-0034", "the layout offset {} is not a multiple of 512", layout.offset);
            ROJORHI_VU(
                layout.rowPitch % 256 == 0 && layout.rowPitch >= rowBytes,
                "VU-rojo-cmd-0035",
                "the row pitch {} is not a multiple of 256 or is below the {} bytes of one row of the region",
                layout.rowPitch,
                rowBytes
            );
            ROJORHI_VU(
                layout.rowsPerImage == 0 || layout.rowsPerImage >= heightBlocks,
                "VU-rojo-cmd-0036",
                "the layout has {} rows per image, below the {} rows of the region",
                layout.rowsPerImage,
                heightBlocks
            );
            [[maybe_unused]] const uint64_t imageBytes = uint64_t{ layout.rowPitch } * (layout.rowsPerImage != 0 ? layout.rowsPerImage : heightBlocks);
            ROJORHI_VU(
                (region.extent.depth == 1 && region.layers == 1) || imageBytes % 512 == 0,
                "VU-rojo-cmd-0037",
                "the region spans several images {} bytes apart, which is not a multiple of 512",
                imageBytes
            );
        }
    } // namespace

    //==================================================================================================================
    void copyBuffer(
        CommandListCore& list,
        [[maybe_unused]] Buffer src,
        [[maybe_unused]] uint64_t srcOffset,
        [[maybe_unused]] Buffer dst,
        [[maybe_unused]] uint64_t dstOffset,
        [[maybe_unused]] uint64_t size
    )
    {
        checkRecording(list);
        noteNativeCall(list);
        ROJORHI_VU(!list.passOpen, "VU-rojo-cmd-0042", "copyBuffer was recorded with a render pass open");
#ifndef NDEBUG
        // The records feed the Debug-tier checks only; reading them takes the pools' locks (ADR 0019).
        const BufferRecord* from = list.device->pool<Buffer>().checked(src, false);
        const BufferRecord* to = list.device->pool<Buffer>().checked(dst, false);
        checkCopySource(*from);
        checkCopyDestination(*to);
        ROJORHI_VU(
            bufferRangeProblem(srcOffset, size, from->size).empty() && bufferRangeProblem(dstOffset, size, to->size).empty(),
            "VU-rojo-cmd-0043",
            "copyBuffer: source {}; destination {}",
            bufferRangeProblem(srcOffset, size, from->size),
            bufferRangeProblem(dstOffset, size, to->size)
        );
        ROJORHI_VU(
            !(src == dst) || srcOffset + size <= dstOffset || dstOffset + size <= srcOffset,
            "VU-rojo-cmd-0044",
            "copyBuffer copies {} bytes within one buffer from {} to {}, and the ranges overlap",
            size,
            srcOffset,
            dstOffset
        );
#endif
    }

    //==================================================================================================================
    void copyBufferToTexture(
        CommandListCore& list,
        [[maybe_unused]] Buffer src,
        [[maybe_unused]] const BufferTextureLayout& layout,
        [[maybe_unused]] Texture dst,
        [[maybe_unused]] const TextureRegion& region
    )
    {
        checkRecording(list);
        noteNativeCall(list);
        ROJORHI_VU(!list.passOpen, "VU-rojo-cmd-0046", "copyBufferToTexture was recorded with a render pass open");
#ifndef NDEBUG
        // The records feed the Debug-tier checks only; reading them takes the pools' locks (ADR 0019).
        const BufferRecord* from = list.device->pool<Buffer>().checked(src, false);
        const TextureRecord* to = list.device->pool<Texture>().checked(dst, false);
        checkNotSwapchainImage(*to, "copyBufferToTexture");
        checkCopySource(*from);
        ROJORHI_VU(to->samples == 1, "VU-rojo-cmd-0066", "copyBufferToTexture: the destination texture \"{}\" has {} samples", to->label, to->samples);
        checkTextureRegion(*to, region);
        checkLayout(*to, layout, region);
        const std::optional<uint64_t> end = layoutEnd(to->format, layout, region);
        ROJORHI_VU(
            end && *end <= from->size,
            "VU-rojo-cmd-0064",
            "the layout addresses bytes {} to {} of the buffer \"{}\", which has {} bytes",
            layout.offset,
            end ? std::to_string(*end) : std::string("beyond 2^64"),
            from->label,
            from->size
        );
#endif
    }

    //==================================================================================================================
    void copyTextureToBuffer(
        CommandListCore& list,
        [[maybe_unused]] Texture src,
        [[maybe_unused]] const TextureRegion& region,
        [[maybe_unused]] Buffer dst,
        [[maybe_unused]] const BufferTextureLayout& layout
    )
    {
        checkRecording(list);
        noteNativeCall(list);
        ROJORHI_VU(!list.passOpen, "VU-rojo-cmd-0049", "copyTextureToBuffer was recorded with a render pass open");
#ifndef NDEBUG
        // The records feed the Debug-tier checks only; reading them takes the pools' locks (ADR 0019).
        const TextureRecord* from = list.device->pool<Texture>().checked(src, false);
        const BufferRecord* to = list.device->pool<Buffer>().checked(dst, false);
        checkNotSwapchainImage(*from, "copyTextureToBuffer");
        checkCopyDestination(*to);
        ROJORHI_VU(from->samples == 1, "VU-rojo-cmd-0068", "copyTextureToBuffer: the source texture \"{}\" has {} samples", from->label, from->samples);
        checkTextureRegion(*from, region);
        checkLayout(*from, layout, region);
        const std::optional<uint64_t> end = layoutEnd(from->format, layout, region);
        ROJORHI_VU(
            end && *end <= to->size,
            "VU-rojo-cmd-0065",
            "the layout addresses bytes {} to {} of the buffer \"{}\", which has {} bytes",
            layout.offset,
            end ? std::to_string(*end) : std::string("beyond 2^64"),
            to->label,
            to->size
        );
#endif
    }

    //==================================================================================================================
    void copyTexture(
        CommandListCore& list,
        [[maybe_unused]] Texture src,
        [[maybe_unused]] const TextureRegion& srcRegion,
        [[maybe_unused]] Texture dst,
        [[maybe_unused]] const TextureRegion& dstRegion
    )
    {
        checkRecording(list);
        noteNativeCall(list);
        ROJORHI_VU(!list.passOpen, "VU-rojo-cmd-0052", "copyTexture was recorded with a render pass open");
#ifndef NDEBUG
        // The records feed the Debug-tier checks only; reading them takes the pools' locks (ADR 0019).
        const TextureRecord* from = list.device->pool<Texture>().checked(src, false);
        const TextureRecord* to = list.device->pool<Texture>().checked(dst, false);
        checkNotSwapchainImage(*from, "copyTexture");
        checkNotSwapchainImage(*to, "copyTexture");
        checkTextureRegion(*from, srcRegion);
        checkTextureRegion(*to, dstRegion);
        const FormatLayout fromBlock = formatLayout(from->format);
        const FormatLayout toBlock = formatLayout(to->format);
        ROJORHI_VU(
            (from->format == to->format || srgbCounterpart(from->format) == to->format) && from->samples == 1 && to->samples == 1 &&
                srcRegion.layers == dstRegion.layers &&
                divCeil(srcRegion.extent.width, fromBlock.blockWidth) == divCeil(dstRegion.extent.width, toBlock.blockWidth) &&
                divCeil(srcRegion.extent.height, fromBlock.blockHeight) == divCeil(dstRegion.extent.height, toBlock.blockHeight) &&
                srcRegion.extent.depth == dstRegion.extent.depth,
            "VU-rojo-cmd-0053",
            "copyTexture between the textures \"{}\" and \"{}\" needs copy-compatible formats, one sample and regions of equal extent in blocks and layers",
            from->label,
            to->label
        );
        ROJORHI_VU(
            !(src == dst) || srcRegion.mip != dstRegion.mip || uint64_t{ srcRegion.layer } + srcRegion.layers <= dstRegion.layer ||
                uint64_t{ dstRegion.layer } + dstRegion.layers <= srcRegion.layer,
            "VU-rojo-cmd-0055",
            "copyTexture within the texture \"{}\" names regions that share a subresource",
            from->label
        );
#endif
    }

    //==================================================================================================================
    void fillBuffer(CommandListCore& list, [[maybe_unused]] Buffer dst, [[maybe_unused]] uint64_t offset, [[maybe_unused]] uint64_t size)
    {
        checkRecording(list);
        noteNativeCall(list);
        ROJORHI_VU(computeCopyProblem(list).empty(), "VU-rojo-cmd-0056", "fillBuffer: {}", computeCopyProblem(list));
#ifndef NDEBUG
        // The records feed the Debug-tier checks only; reading them takes the pools' locks (ADR 0019).
        const BufferRecord* to = list.device->pool<Buffer>().checked(dst, false);
        ROJORHI_VU(bufferRangeProblem(offset, size, to->size).empty(), "VU-rojo-cmd-0057", "fillBuffer: {}", bufferRangeProblem(offset, size, to->size));
        ROJORHI_VU(
            hasUsage(to->usage, BufferUsage::ShaderWrite), "VU-rojo-cmd-0058", "fillBuffer: the buffer \"{}\" was not created with ShaderWrite", to->label
        );
#endif
    }

    //==================================================================================================================
    void clearTexture(
        CommandListCore& list, [[maybe_unused]] Texture texture, [[maybe_unused]] const SubresourceRange& range, [[maybe_unused]] const ClearValue& value
    )
    {
        checkRecording(list);
        noteNativeCall(list);
        ROJORHI_VU(computeCopyProblem(list).empty(), "VU-rojo-cmd-0060", "clearTexture: {}", computeCopyProblem(list));
#ifndef NDEBUG
        // The records feed the Debug-tier checks only; reading them takes the pools' locks (ADR 0019).
        const TextureRecord* target = list.device->pool<Texture>().checked(texture, false);
        checkNotSwapchainImage(*target, "clearTexture");
        ROJORHI_VU(value.depth >= 0.0f && value.depth <= 1.0f, "VU-rojo-cmd-0059", "clearTexture: the clear depth {} is outside [0, 1]", value.depth);
        checkSubresourceRange(*target, range);
        const bool attachment = hasUsage(target->usage, TextureUsage::RenderTarget | TextureUsage::DepthStencil);
        const bool storage = hasUsage(target->usage, TextureUsage::Storage);
        ROJORHI_VU(
            list.type == QueueType::Compute ? storage && !attachment : attachment || storage,
            "VU-rojo-cmd-0061",
            "clearTexture: the texture \"{}\" has a usage that a {} list cannot clear",
            target->label,
            list.type == QueueType::Compute ? "Compute" : "Graphics"
        );
#endif
    }
} // namespace rojoRHI::core
