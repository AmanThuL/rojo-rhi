//----------------------------------------------------------------------------------------------------------------------
/// @file Textures.cpp
/// @brief Implements the checks of the texture and view rules of chapter 04.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Textures.h"

#include "Base/Assert.h"
#include "Core/Formats.h"
#include "Core/Heaps.h"
#include "Core/Label.h"

#include <algorithm>
#include <bit>
#include <format>
#include <utility>

namespace rojoRHI::core {
    namespace {
        //==============================================================================================================
        [[maybe_unused]] bool hasUsage(TextureUsage usage, TextureUsage flag)
        {
            return (usage & flag) != TextureUsage::None;
        }

        //==============================================================================================================
        [[maybe_unused]] uint32_t dimensionLimit(const Limits& limits, TextureType type)
        {
            switch (type)
            {
            case TextureType::Tex1D:
            case TextureType::Tex1DArray: return limits.maxTextureDimension1D;
            case TextureType::Tex2D:
            case TextureType::Tex2DArray: return limits.maxTextureDimension2D;
            case TextureType::Tex3D: return limits.maxTextureDimension3D;
            case TextureType::Cube:
            case TextureType::CubeArray: return limits.maxTextureDimensionCube;
            }
            return 0;
        }

        //==============================================================================================================
        [[maybe_unused]] bool isCube(TextureType type)
        {
            return type == TextureType::Cube || type == TextureType::CubeArray;
        }

        //==============================================================================================================
        [[maybe_unused]] bool layersMatchType(const TextureDesc& desc)
        {
            switch (desc.type)
            {
            case TextureType::Tex1D:
            case TextureType::Tex2D:
            case TextureType::Tex3D: return desc.layers == 1;
            case TextureType::Cube: return desc.layers == 6;
            case TextureType::CubeArray: return desc.layers > 0 && desc.layers % 6 == 0;
            case TextureType::Tex1DArray:
            case TextureType::Tex2DArray: return desc.layers >= 1;
            }
            return false;
        }

        //==============================================================================================================
        /// Stores `a * b` in `product` and returns true, or returns false when it does not fit 64 bits.
        [[maybe_unused]] bool multiply(uint64_t a, uint64_t b, uint64_t& product)
        {
            if (a != 0 && b > UINT64_MAX / a)
            {
                return false;
            }
            product = a * b;
            return true;
        }

        //==============================================================================================================
        /// Checks the strides and size of the initial data of the subresource at mip level `mip`.
        void checkTextureData(const TextureRecord& texture, const TextureData& data, uint32_t mip)
        {
            if (data.bytes.empty())
            {
                return;
            }
            const FormatLayout layout = formatLayout(texture.format);
            const Extent3D extent = mipExtent(texture.extent, mip);
            [[maybe_unused]] const uint64_t packedRow = (uint64_t{ extent.width } + layout.blockWidth - 1) / layout.blockWidth * layout.blockBytes;
            [[maybe_unused]] const uint64_t rows = (uint64_t{ extent.height } + layout.blockHeight - 1) / layout.blockHeight;
            [[maybe_unused]] const bool volume = texture.type == TextureType::Tex3D;
            ROJORHI_VU(
                data.bytesPerRow >= packedRow && data.bytesPerRow % layout.blockBytes == 0,
                "VU-rojo-texture-0013",
                "bytesPerRow {} is below the packed row of {} bytes or not a multiple of the {}-byte texel or block",
                data.bytesPerRow,
                packedRow,
                layout.blockBytes
            );
            [[maybe_unused]] uint64_t sliceBytes = 0;
            [[maybe_unused]] const bool sliceFits = multiply(data.bytesPerRow, rows, sliceBytes);
            ROJORHI_VU(
                volume ? (sliceFits && data.bytesPerImage >= sliceBytes) : data.bytesPerImage == 0,
                "VU-rojo-texture-0014",
                "bytesPerImage {} does not suit {} rows of {} bytes in a {} texture",
                data.bytesPerImage,
                rows,
                data.bytesPerRow,
                volume ? "Tex3D" : "non-Tex3D"
            );
            // The last row of the last slice is not padded to the stride; a product or sum that wraps is never enough.
            [[maybe_unused]] uint64_t slices = 0;
            [[maybe_unused]] uint64_t body = 0;
            [[maybe_unused]] const bool impliedFits = (!volume || multiply(data.bytesPerImage, extent.depth - 1, slices)) &&
                                                      multiply(data.bytesPerRow, rows - 1, body) && slices <= UINT64_MAX - body &&
                                                      slices + body <= UINT64_MAX - packedRow;
            [[maybe_unused]] const uint64_t implied = impliedFits ? slices + body + packedRow : UINT64_MAX;
            ROJORHI_VU(
                impliedFits && data.bytes.size() >= implied,
                "VU-rojo-texture-0015",
                "{} bytes of initial data are given but the strides imply more than {}",
                data.bytes.size(),
                impliedFits ? implied : UINT64_MAX
            );
        }

        //==============================================================================================================
        /// The layers a view of `range` covers once `kAllLayers` is resolved against `texture`.
        [[maybe_unused]] uint64_t resolvedLayers(const TextureRecord& texture, const SubresourceRange& range)
        {
            return range.layerCount == kAllLayers ? uint64_t{ texture.layers } - std::min(range.baseLayer, texture.layers) : range.layerCount;
        }

        //==============================================================================================================
        /// The mips a view of `range` covers once `kAllMips` is resolved against `texture`.
        [[maybe_unused]] uint64_t resolvedMips(const TextureRecord& texture, const SubresourceRange& range)
        {
            return range.mipCount == kAllMips ? uint64_t{ texture.mips } - std::min(range.baseMip, texture.mips) : range.mipCount;
        }

        //==============================================================================================================
        [[maybe_unused]] bool viewTypeFits(TextureType view, TextureType texture)
        {
            switch (view)
            {
            case TextureType::Tex1D:
            case TextureType::Tex1DArray: return texture == TextureType::Tex1D || texture == TextureType::Tex1DArray;
            case TextureType::Tex2D:
            case TextureType::Tex2DArray: return texture == TextureType::Tex2D || texture == TextureType::Tex2DArray || isCube(texture);
            case TextureType::Cube: return isCube(texture);
            case TextureType::CubeArray: return texture == TextureType::CubeArray;
            case TextureType::Tex3D: return texture == TextureType::Tex3D;
            }
            return false;
        }

        //==============================================================================================================
        [[maybe_unused]] bool viewLayersFitType(TextureType view, uint64_t layers)
        {
            switch (view)
            {
            case TextureType::Tex1D:
            case TextureType::Tex2D:
            case TextureType::Tex3D: return layers == 1;
            case TextureType::Cube: return layers == 6;
            case TextureType::CubeArray: return layers % 6 == 0;
            case TextureType::Tex1DArray:
            case TextureType::Tex2DArray: return true;
            }
            return false;
        }

        //==============================================================================================================
        [[maybe_unused]] bool aspectFits(const TextureRecord& texture, const TextureViewDesc& desc)
        {
            if (!isDepthFormat(texture.format))
            {
                return desc.aspect == TextureAspect::Color;
            }
            const bool formatKept = desc.format == Format::Undefined || desc.format == texture.format;
            const bool depth = desc.aspect == TextureAspect::Depth;
            const bool stencil = desc.aspect == TextureAspect::Stencil && texture.format == Format::D32FloatS8 && desc.usage == TextureUsage::Sampled;
            return formatKept && (depth || stencil);
        }

        //==============================================================================================================
        [[maybe_unused]] bool rangeNonEmpty(const SubresourceRange& range)
        {
            return range.mipCount != 0 && range.layerCount != 0;
        }

        //==============================================================================================================
        [[maybe_unused]] bool rangeStartsInside(const TextureRecord& texture, const SubresourceRange& range)
        {
            return range.baseMip < texture.mips && range.baseLayer < texture.layers;
        }

        //==============================================================================================================
        [[maybe_unused]] bool rangeInside(const TextureRecord& texture, const SubresourceRange& range)
        {
            return (range.mipCount == kAllMips || uint64_t{ range.baseMip } + range.mipCount <= texture.mips) &&
                   (range.layerCount == kAllLayers || uint64_t{ range.baseLayer } + range.layerCount <= texture.layers);
        }

        //==============================================================================================================
        [[maybe_unused]] bool usageFits(const TextureRecord& texture, const TextureViewDesc& desc)
        {
            return (desc.usage == TextureUsage::Sampled || desc.usage == TextureUsage::Storage) && hasUsage(texture.usage, desc.usage);
        }

        //==============================================================================================================
        [[maybe_unused]] bool formatFits(const TextureRecord& texture, const TextureViewDesc& desc)
        {
            const Format counterpart = srgbCounterpart(texture.format);
            return desc.format == Format::Undefined || desc.format == texture.format || (counterpart != Format::Undefined && desc.format == counterpart);
        }

        //==============================================================================================================
        [[maybe_unused]] bool storageFits(const TextureRecord& texture, const TextureViewDesc& desc)
        {
            return desc.usage != TextureUsage::Storage || (resolvedMips(texture, desc.range) == 1 && texture.samples == 1);
        }

        //==============================================================================================================
        [[maybe_unused]] bool capabilityFits(const DeviceCore& core, const TextureRecord& texture, const TextureViewDesc& desc)
        {
            const FormatCapabilities capabilities = core.formatCapabilities(desc.format == Format::Undefined ? texture.format : desc.format);
            return desc.usage == TextureUsage::Sampled ? capabilities.sampled : (capabilities.storageRead && capabilities.storageWrite);
        }

        //==============================================================================================================
        /// Returns true when `desc` obeys every rule of a `TextureViewDesc` for `texture`; the same rules `checkViewDesc`
        /// reports one by one, asked as one question by `writeView`.
        [[maybe_unused]] bool viewDescFits(const DeviceCore& core, const TextureRecord& texture, const TextureViewDesc& desc)
        {
            return rangeNonEmpty(desc.range) && rangeInside(texture, desc.range) && rangeStartsInside(texture, desc.range) && usageFits(texture, desc) &&
                   viewTypeFits(desc.type, texture.type) && viewLayersFitType(desc.type, resolvedLayers(texture, desc.range)) && formatFits(texture, desc) &&
                   storageFits(texture, desc) && aspectFits(texture, desc) && capabilityFits(core, texture, desc);
        }

        //==============================================================================================================
        void checkViewDesc([[maybe_unused]] const DeviceCore& core, const TextureRecord& texture, const TextureViewDesc& desc)
        {
            checkSubresourceRange(texture, desc.range);
            ROJORHI_VU(
                usageFits(texture, desc),
                "VU-rojo-texture-0022",
                "the view usage is not exactly Sampled or Storage, or the texture \"{}\" was created without it",
                texture.label
            );
            ROJORHI_VU(
                viewTypeFits(desc.type, texture.type),
                "VU-rojo-texture-0023",
                "the view type is not compatible with the type of the texture \"{}\"",
                texture.label
            );
            ROJORHI_VU(
                viewLayersFitType(desc.type, resolvedLayers(texture, desc.range)),
                "VU-rojo-texture-0024",
                "the view covers {} layers, which its type does not allow",
                resolvedLayers(texture, desc.range)
            );
            ROJORHI_VU(
                formatFits(texture, desc),
                "VU-rojo-texture-0025",
                "the view format is not the format of the texture \"{}\" or its sRGB or linear counterpart",
                texture.label
            );
            ROJORHI_VU(
                storageFits(texture, desc),
                "VU-rojo-texture-0026",
                "a Storage view covers {} mips of a texture with {} samples",
                resolvedMips(texture, desc.range),
                texture.samples
            );
            ROJORHI_VU(aspectFits(texture, desc), "VU-rojo-texture-0027", "the view aspect does not fit the format of the texture \"{}\"", texture.label);
            ROJORHI_VU(
                capabilityFits(core, texture, desc),
                "VU-rojo-format-0003",
                "the device does not support the format of the view for {} access",
                desc.usage == TextureUsage::Sampled ? "Sampled" : "Storage"
            );
        }
    } // namespace

    //==================================================================================================================
    Extent3D mipExtent(const Extent3D& extent, uint32_t mip)
    {
        return Extent3D{ std::max(extent.width >> mip, 1u), std::max(extent.height >> mip, 1u), std::max(extent.depth >> mip, 1u) };
    }

    //==================================================================================================================
    void checkTextureDesc([[maybe_unused]] const DeviceCore& core, [[maybe_unused]] const TextureDesc& desc)
    {
        [[maybe_unused]] const Capabilities& capabilities = core.capabilities();
        [[maybe_unused]] const Limits& limits = capabilities.limits;
        ROJORHI_VU(desc.format != Format::Undefined, "VU-rojo-texture-0001", "a texture has Format::Undefined");
        // Asked after VU-rojo-texture-0001, because the query rejects Undefined as VU-rojo-format-0005.
        [[maybe_unused]] const FormatCapabilities formatCaps = core.formatCapabilities(desc.format);
        [[maybe_unused]] const uint32_t limit = dimensionLimit(limits, desc.type);
        [[maybe_unused]] const auto inRange = [limit](uint32_t value) { return value >= 1 && value <= limit; };
        ROJORHI_VU(
            inRange(desc.extent.width) && inRange(desc.extent.height) && inRange(desc.extent.depth),
            "VU-rojo-texture-0002",
            "extent {}x{}x{} is not in 1 to {} on every axis",
            desc.extent.width,
            desc.extent.height,
            desc.extent.depth,
            limit
        );
        [[maybe_unused]] const bool oneRow = desc.type == TextureType::Tex1D || desc.type == TextureType::Tex1DArray;
        ROJORHI_VU(
            (!oneRow || desc.extent.height == 1) && (desc.type == TextureType::Tex3D || desc.extent.depth == 1),
            "VU-rojo-texture-0003",
            "extent {}x{}x{} does not suit the texture type",
            desc.extent.width,
            desc.extent.height,
            desc.extent.depth
        );
        ROJORHI_VU(
            !isCube(desc.type) || desc.extent.width == desc.extent.height,
            "VU-rojo-texture-0004",
            "a cube texture is {}x{}, not square",
            desc.extent.width,
            desc.extent.height
        );
        ROJORHI_VU(layersMatchType(desc), "VU-rojo-texture-0005", "{} layers do not suit the texture type", desc.layers);
        ROJORHI_VU(desc.layers <= limits.maxTextureLayers, "VU-rojo-texture-0039", "{} layers exceed the limit of {}", desc.layers, limits.maxTextureLayers);
        [[maybe_unused]] const uint32_t largest = std::max({ desc.extent.width, desc.extent.height, desc.extent.depth });
        [[maybe_unused]] const uint32_t mipLimit = static_cast<uint32_t>(std::bit_width(largest));
        ROJORHI_VU(
            desc.mips >= 1 && desc.mips <= mipLimit,
            "VU-rojo-texture-0006",
            "{} mips are not in 1 to {} for the largest extent {}",
            desc.mips,
            mipLimit,
            largest
        );
        ROJORHI_VU(samplesSupported(desc.samples, formatCaps), "VU-rojo-texture-0007", "{} samples are not supported for the format", desc.samples);
        ROJORHI_VU(
            desc.samples == 1 || ((desc.type == TextureType::Tex2D || desc.type == TextureType::Tex2DArray) && desc.mips == 1 &&
                                  !hasUsage(desc.usage, TextureUsage::Storage | TextureUsage::Sparse)),
            "VU-rojo-texture-0008",
            "a multisampled texture is not a single-mip 2D texture without Storage or Sparse usage"
        );
        ROJORHI_VU(
            desc.usage != TextureUsage::None && !(hasUsage(desc.usage, TextureUsage::RenderTarget) && hasUsage(desc.usage, TextureUsage::DepthStencil)),
            "VU-rojo-texture-0009",
            "the usage is empty or holds both RenderTarget and DepthStencil"
        );
        ROJORHI_VU(
            (!hasUsage(desc.usage, TextureUsage::DepthStencil) || formatCaps.depthAttachment) &&
                (!hasUsage(desc.usage, TextureUsage::RenderTarget) || formatCaps.colorAttachment) &&
                (!hasUsage(desc.usage, TextureUsage::Storage) || (formatCaps.storageRead && formatCaps.storageWrite)),
            "VU-rojo-texture-0010",
            "a usage bit is not supported for the format"
        );
        ROJORHI_VU(
            !(desc.type == TextureType::Tex3D && hasUsage(desc.usage, TextureUsage::DepthStencil)),
            "VU-rojo-texture-0011",
            "a Tex3D texture has DepthStencil usage"
        );
        ROJORHI_VU(
            !hasUsage(desc.usage, TextureUsage::Sparse) || capabilities.placementSparse,
            "VU-rojo-texture-0012",
            "Sparse is set but the device reports no sparse resources"
        );
        ROJORHI_VU(
            (!hasUsage(desc.usage, TextureUsage::Sampled) || formatCaps.sampled) && (!hasUsage(desc.usage, TextureUsage::Sparse) || formatCaps.sparse),
            "VU-rojo-format-0003",
            "the device does not support the format for Sampled or Sparse use"
        );
        ROJORHI_VU(
            !isBlockCompressed(desc.format) || (desc.extent.width % 4 == 0 && desc.extent.height % 4 == 0),
            "VU-rojo-format-0004",
            "a block-compressed texture is {}x{}, not a multiple of 4 on both axes",
            desc.extent.width,
            desc.extent.height
        );
    }

    //==================================================================================================================
    void checkTextureRequirementsQuery(const DeviceCore& core, const TextureDesc& desc)
    {
        checkTextureDesc(core, desc);
        ROJORHI_VU(!hasUsage(desc.usage, TextureUsage::Sparse), "VU-rojo-heap-0002", "placement requirements are requested for a Sparse texture");
    }

    //==================================================================================================================
    TextureRecord prepareTexture(
        DeviceCore& core, const TextureDesc& desc, std::span<const TextureData> initial, const Placement& placement, const PlacementRequirements& requirements
    )
    {
        checkTextureDesc(core, desc);
        ROJORHI_VU(
            initial.empty() || initial.size() == uint64_t{ desc.mips } * desc.layers,
            "VU-rojo-texture-0018",
            "{} initial entries are given for {} mips of {} layers",
            initial.size(),
            desc.mips,
            desc.layers
        );
        ROJORHI_VU(initial.empty() || !hasUsage(desc.usage, TextureUsage::Sparse), "VU-rojo-texture-0019", "initial data is given for a Sparse texture");
        ROJORHI_VU(
            initial.empty() || hasUsage(desc.usage, TextureUsage::Sampled), "VU-rojo-texture-0020", "initial data is given for a texture without Sampled usage"
        );
        ROJORHI_VU(initial.empty() || desc.samples == 1, "VU-rojo-texture-0021", "initial data is given for a texture with {} samples", desc.samples);
        ROJORHI_VU(
            initial.empty() || desc.format != Format::D32FloatS8, "VU-rojo-texture-0040", "initial data is given for D32FloatS8, which has no packed texel size"
        );
        TextureRecord record;
        record.heap = placement.heap;
        record.label = resolveLabel(desc.label, placement.heap.bits != 0 ? "rojorhi.texture.placed.unnamed" : "rojorhi.texture.unnamed");
        record.type = desc.type;
        record.format = desc.format;
        record.extent = desc.extent;
        record.mips = desc.mips;
        record.layers = desc.layers;
        record.samples = desc.samples;
        record.usage = desc.usage;
        for (size_t index = 0; index < initial.size(); ++index)
        {
            checkTextureData(record, initial[index], static_cast<uint32_t>(index % std::max(desc.mips, 1u)));
            record.initialBytes += initial[index].bytes.size();
        }
        if (placement.heap.bits != 0)
        {
            checkPlacement(
                checkedHeap(core, placement.heap), placement, requirements, PlacedResource{ Memory::Gpu, true, hasUsage(desc.usage, TextureUsage::Sparse) }
            );
        }
        return record;
    }

    //==================================================================================================================
    Texture addTexture(DeviceCore& core, TextureRecord record)
    {
        if (record.heap.bits != 0)
        {
            addPlacedResource(core, record.heap);
        }
        return core.pool<Texture>().allocate(std::move(record));
    }

    //==================================================================================================================
    void destroyTexture(DeviceCore& core, Texture texture)
    {
        // A bad handle is left to the destroy below, which reports VU-rojo-core-0001 to 0003 and releases nothing. The view
        // count is read under the pool's lock, as it is written.
        Heap heap = {};
        [[maybe_unused]] uint64_t views = 0;
        [[maybe_unused]] std::string label;
        [[maybe_unused]] bool swapchainImage = false;
        [[maybe_unused]] const LookupStatus status = core.pool<Texture>().update(texture, [&](TextureRecord& record) {
            heap = record.heap;
            views = record.views;
            label = record.label;
            swapchainImage = record.swapchainImage;
        });
        ROJORHI_VU(!swapchainImage, "VU-rojo-present-0013", "the swapchain image \"{}\" is destroyed; its swapchain owns it", label);
        ROJORHI_VU(
            status != LookupStatus::Ok || views == 0,
            "VU-rojo-texture-0028",
            "the texture \"{}\" is destroyed while {} view slots still hold a view of it",
            label,
            views
        );
        core.destroy(texture);
        if (heap.bits != 0)
        {
            removePlacedResource(core, heap);
        }
    }

    //==================================================================================================================
    void checkNotSwapchainImage([[maybe_unused]] const TextureRecord& texture, [[maybe_unused]] std::string_view use)
    {
        ROJORHI_VU(
            !texture.swapchainImage,
            "VU-rojo-present-0012",
            "the swapchain image \"{}\" is used by {}; it is used only as a color attachment and in texture barriers",
            texture.label,
            use
        );
    }

    //==================================================================================================================
    void checkSubresourceRange([[maybe_unused]] const TextureRecord& texture, [[maybe_unused]] const SubresourceRange& range)
    {
        ROJORHI_VU(rangeNonEmpty(range), "VU-rojo-texture-0016", "a subresource range has {} mips and {} layers", range.mipCount, range.layerCount);
        ROJORHI_VU(
            rangeInside(texture, range),
            "VU-rojo-texture-0017",
            "the range of {} mips from {} and {} layers from {} lies outside the texture \"{}\" of {} mips and {} layers",
            range.mipCount,
            range.baseMip,
            range.layerCount,
            range.baseLayer,
            texture.label,
            texture.mips,
            texture.layers
        );
        ROJORHI_VU(
            rangeStartsInside(texture, range),
            "VU-rojo-texture-0041",
            "the range starts at mip {} and layer {}, which is not inside the texture \"{}\" of {} mips and {} layers",
            range.baseMip,
            range.baseLayer,
            texture.label,
            texture.mips,
            texture.layers
        );
    }

    //==================================================================================================================
    Result<ViewIndex> createView(DeviceCore& core, Texture texture, const TextureViewDesc& desc)
    {
        TextureRecord* record = core.pool<Texture>().checked(texture, false);
        ROJORHI_INTERNAL_ASSERT(record != nullptr, "a checked lookup that does not allow null returned null");
        // The record's address is stable while the texture lives, and its description never changes.
        checkNotSwapchainImage(*record, "createView");
        checkViewDesc(core, *record, desc);
        const std::optional<uint32_t> slot = core.views().allocate(core.capabilities().maxViews, texture.bits);
        if (!slot)
        {
            return std::unexpected(Error{ ErrorCategory::OutOfMemory, std::format("the view heap of {} slots is full", core.capabilities().maxViews) });
        }
        core.pool<Texture>().update(texture, [](TextureRecord& held) { ++held.views; });
        return static_cast<ViewIndex>(*slot);
    }

    //==================================================================================================================
    void writeView(DeviceCore& core, IndexRange range, uint32_t index, Texture texture, [[maybe_unused]] const TextureViewDesc& desc)
    {
        ROJORHI_VU(
            range.kind == IndexRange::Kind::Views && index < range.count,
            "VU-rojo-texture-0033",
            "writeView names slot {} of a range of {} {} slots",
            index,
            range.count,
            range.kind == IndexRange::Kind::Views ? "view" : "sampler"
        );
        [[maybe_unused]] TextureRecord* record = core.pool<Texture>().checked(texture, false);
        ROJORHI_INTERNAL_ASSERT(record != nullptr, "a checked lookup that does not allow null returned null");
        checkNotSwapchainImage(*record, "writeView");
        ROJORHI_VU(
            viewDescFits(core, *record, desc),
            "VU-rojo-texture-0034",
            "the view descriptor breaks a TextureViewDesc rule for the texture \"{}\"; createView reports which one",
            record->label
        );
        ROJORHI_VU(
            core.views().holdsRange(range.base, range.count),
            "VU-rojo-texture-0031",
            "writeView names the range of {} slots from {}, which a reserve call did not return or which was released",
            range.count,
            range.base
        );
        // Count the new view first, so the texture never reads as unviewed while a slot is about to hold it. A range released
        // by a racing call after the check above leaves the slot unwritten and the count restored.
        core.pool<Texture>().update(texture, [](TextureRecord& held) { ++held.views; });
        const std::optional<uint64_t> previous = core.views().replaceOwner(range.base + index, texture.bits);
        if (!previous)
        {
            core.pool<Texture>().update(texture, [](TextureRecord& held) { --held.views; });
            return;
        }
        if (*previous != 0)
        {
            core.pool<Texture>().update(Texture{ *previous }, [](TextureRecord& held) {
                ROJORHI_INTERNAL_ASSERT(held.views > 0, "a texture counts fewer views than hold it");
                --held.views;
            });
        }
    }

    //==================================================================================================================
    void destroyView(DeviceCore& core, ViewIndex view)
    {
        const std::optional<uint64_t> owner = core.views().release(std::to_underlying(view), [&core] { return core.releaseFrame(); });
        ROJORHI_VU(
            owner.has_value(), "VU-rojo-device-0010", "the view index {} was not returned by createView or was already destroyed", std::to_underlying(view)
        );
        if (owner)
        {
            core.pool<Texture>().update(Texture{ *owner }, [](TextureRecord& held) {
                ROJORHI_INTERNAL_ASSERT(held.views > 0, "a texture counts fewer views than hold it");
                --held.views;
            });
        }
    }
} // namespace rojoRHI::core
