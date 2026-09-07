//----------------------------------------------------------------------------------------------------------------------
/// @file Validate.cpp
/// @brief Implements backend-neutral validation for RHI descriptors and pass attachments.
//----------------------------------------------------------------------------------------------------------------------
#include "RHI/Validate.h"

#include "RHI/Indirect.h"

#include <limits>
#include <string>

namespace lmx::rhi {
namespace {

//======================================================================================================================
Result<void> invalid(const char* message) {
    return std::unexpected(Error{ErrorCode::InvalidDesc, message});
}

constexpr uint32_t kMaxTextureDimension2D = 16384;

//======================================================================================================================
// Reject non-renderable formats before Metal's pipeline and layer validators abort. sRGB encodes
// on write; block compression cannot run per fragment.
bool isColorRenderableFormat(Format format) {
    return format == Format::BGRA8Unorm || format == Format::RGBA8Unorm ||
           format == Format::RGBA8Unorm_sRGB || format == Format::RGBA16Float ||
           format == Format::RG16Float || format == Format::R8Unorm;
}

//======================================================================================================================
// The window path is intentionally SDR. RGBA16Float is a valid offscreen render target, but the
// swapchain does not configure CAMetalLayer for an extended-range presentation mode, so accepting
// it here would advertise a presentation capability the backend has never established or tested.
bool isSwapchainFormat(Format format) {
    return format == Format::BGRA8Unorm || format == Format::RGBA8Unorm ||
           format == Format::RGBA8Unorm_sRGB;
}

//======================================================================================================================
// Centralize the depth-format set used by pass and pipeline validation.
bool isDepthFormat(Format format) {
    return format == Format::D32Float;
}

//======================================================================================================================
// Integer shifts avoid floating-point floor error for non-power-of-two mip chains.
uint32_t maxMipLevels(uint32_t width, uint32_t height) {
    uint32_t extent = width > height ? width : height;
    uint32_t levels = 1;
    while (extent > 1) {
        extent >>= 1;
        ++levels;
    }
    return levels;
}

// Metal aborts for anisotropy outside [1, 16] and provides no error object.
constexpr uint32_t kMinAnisotropy = 1;
constexpr uint32_t kMaxAnisotropy = 16;

//======================================================================================================================
std::string extentOf(uint64_t width, uint64_t height) {
    return std::to_string(width) + "x" + std::to_string(height);
}

//======================================================================================================================
std::string extentOf(const Texture& texture) {
    return extentOf(texture.width(), texture.height());
}

//======================================================================================================================
// Whether two half-open intervals share a byte or a texel. Sizes are added in 64 bits so a texel
// interval taken from 32-bit extents cannot wrap.
bool overlaps(uint64_t firstStart, uint64_t firstSize, uint64_t secondStart, uint64_t secondSize) {
    return firstStart < secondStart + secondSize && secondStart < firstStart + firstSize;
}

//======================================================================================================================
bool checkedMultiply(uint64_t a, uint64_t b, uint64_t& result) {
    if (a != 0 && b > std::numeric_limits<uint64_t>::max() / a) {
        return false;
    }
    result = a * b;
    return true;
}

//======================================================================================================================
bool checkedAdd(uint64_t a, uint64_t b, uint64_t& result) {
    if (b > std::numeric_limits<uint64_t>::max() - a) {
        return false;
    }
    result = a + b;
    return true;
}

//======================================================================================================================
// Two formats belong to one family when they describe the same bits and differ only in the transfer
// function applied on access, which is the only reinterpretation a view may perform: anything else
// would reinterpret the memory itself.
bool isSameFormatFamily(Format a, Format b) {
    const auto linearOf = [](Format format) {
        switch (format) {
        case Format::RGBA8Unorm_sRGB:
            return Format::RGBA8Unorm;
        case Format::BC1Unorm_sRGB:
            return Format::BC1Unorm;
        default:
            return format;
        }
    };
    return linearOf(a) == linearOf(b);
}

//======================================================================================================================
// Resolves a range's "everything from here on" sentinel against a concrete extent.
uint32_t resolveCount(uint32_t count, uint32_t sentinel, uint32_t base, uint32_t total) {
    return count == sentinel ? (base < total ? total - base : 0) : count;
}

} // namespace

//======================================================================================================================
// Omitting default lets -Wswitch catch a newly added format that has not decided its answer here.
uint32_t bytesPerPixel(Format format) {
    switch (format) {
    case Format::BGRA8Unorm:
    case Format::RGBA8Unorm:
    case Format::RGBA8Unorm_sRGB:
        return 4;
    case Format::RG16Float:
        return 4;
    case Format::R8Unorm:
        return 1;
    case Format::RGBA16Float:
        return 8;
    // A BC1 block covers 4x4 texels, so "bytes per pixel" is not expressible; D32Float is a packed
    // depth format with no readback caller.
    case Format::BC1Unorm:
    case Format::BC1Unorm_sRGB:
    case Format::D32Float:
    case Format::Unknown:
        break;
    }
    return 0;
}

//======================================================================================================================
// The read-write set Apple silicon supports, intersected with this RHI's formats: the sRGB and
// block-compressed members are excluded because a storage access performs no decode, and the packed
// depth format has none to expose. The two- and single-channel formats are left out for a different
// reason -- every producer of one writes it as a colour attachment, so no caller needs the
// read-write view the hardware would allow.
bool isStorageFormat(Format format) {
    return format == Format::RGBA8Unorm || format == Format::RGBA16Float;
}

//======================================================================================================================
Result<void> validate(const BufferDesc& desc) {
    if (desc.size == 0) {
        return invalid("BufferDesc.size must be greater than zero");
    }
    return {};
}

//======================================================================================================================
Result<void> validate(const TextureDesc& desc) {
    if (desc.width == 0) {
        return invalid("TextureDesc.width must be greater than zero");
    }
    if (desc.width > kMaxTextureDimension2D) {
        return invalid("TextureDesc.width exceeds the maximum 2D texture dimension (16384)");
    }
    if (desc.height == 0) {
        return invalid("TextureDesc.height must be greater than zero");
    }
    if (desc.height > kMaxTextureDimension2D) {
        return invalid("TextureDesc.height exceeds the maximum 2D texture dimension (16384)");
    }
    if (desc.format == Format::Unknown) {
        return invalid("TextureDesc.format must not be Format::Unknown");
    }
    // Metal silently ignores cube height and creates square faces, so reject oblong descriptors.
    if (desc.kind == TextureKind::Cube && desc.width != desc.height) {
        return invalid("TextureDesc.kind == Cube requires width == height (a cube's faces are "
                       "square)");
    }
    if (desc.mipLevels == 0) {
        return invalid("TextureDesc.mipLevels must be at least 1 (level 0 always exists)");
    }
    if (desc.mipLevels > maxMipLevels(desc.width, desc.height)) {
        return invalid("TextureDesc.mipLevels exceeds the chain the extent allows "
                       "(floor(log2(max(width, height))) + 1)");
    }
    if (!desc.renderTarget && !desc.sampled && !desc.storageRead && !desc.storageWrite &&
        !desc.cpuReadback) {
        return invalid("TextureDesc has no usage: set at least one of renderTarget, sampled, "
                       "storageRead, storageWrite, or cpuReadback");
    }
    if ((desc.storageRead || desc.storageWrite) && !isStorageFormat(desc.format)) {
        return invalid("TextureDesc storage usage requires a format the hardware can read and "
                       "write without conversion (RGBA8Unorm or RGBA16Float); sRGB, "
                       "block-compressed, depth, two-channel, and single-channel formats are "
                       "not among them");
    }
    if (desc.renderTarget && !isColorRenderableFormat(desc.format) && !isDepthFormat(desc.format)) {
        return invalid("TextureDesc.renderTarget requires a color-renderable or depth format");
    }
    if (desc.cpuReadback && bytesPerPixel(desc.format) == 0) {
        return invalid("TextureDesc.cpuReadback: readback has no packed texel size for this "
                       "format");
    }
    // The readback API cannot select cube faces and would otherwise return only slice zero.
    if (desc.cpuReadback && desc.kind == TextureKind::Cube) {
        return invalid("TextureDesc.cpuReadback: readback returns a single image, so it cannot "
                       "express a Cube's six faces");
    }
    return {};
}

//======================================================================================================================
Result<void> validateSubresourceRange(const Texture& texture,
                                      const TextureSubresourceRange& range) {
    const uint32_t mipLevels = texture.mipLevels();
    const uint32_t arrayLayers = texture.arrayLayers();
    if (range.baseMipLevel >= mipLevels) {
        return std::unexpected(
            Error{ErrorCode::InvalidDesc,
                  "TextureSubresourceRange.baseMipLevel " + std::to_string(range.baseMipLevel) +
                      " is outside the texture's " + std::to_string(mipLevels) + " mip level(s)"});
    }
    if (range.baseArrayLayer >= arrayLayers) {
        return std::unexpected(
            Error{ErrorCode::InvalidDesc,
                  "TextureSubresourceRange.baseArrayLayer " + std::to_string(range.baseArrayLayer) +
                      " is outside the texture's " + std::to_string(arrayLayers) + " layer(s)"});
    }
    const uint32_t mipCount =
        resolveCount(range.mipLevelCount, kAllMipLevels, range.baseMipLevel, mipLevels);
    const uint32_t layerCount =
        resolveCount(range.arrayLayerCount, kAllArrayLayers, range.baseArrayLayer, arrayLayers);
    if (mipCount == 0 || layerCount == 0) {
        return invalid("TextureSubresourceRange must cover at least one mip level and one array "
                       "layer (use kAllMipLevels/kAllArrayLayers for the whole resource)");
    }
    // Compare against the remaining extent so an enormous count cannot overflow the sum.
    if (mipCount > mipLevels - range.baseMipLevel) {
        return std::unexpected(Error{ErrorCode::InvalidDesc,
                                     "TextureSubresourceRange covers " + std::to_string(mipCount) +
                                         " mip level(s) from level " +
                                         std::to_string(range.baseMipLevel) +
                                         ", past the texture's " + std::to_string(mipLevels)});
    }
    if (layerCount > arrayLayers - range.baseArrayLayer) {
        return std::unexpected(Error{ErrorCode::InvalidDesc,
                                     "TextureSubresourceRange covers " +
                                         std::to_string(layerCount) + " layer(s) from layer " +
                                         std::to_string(range.baseArrayLayer) +
                                         ", past the texture's " + std::to_string(arrayLayers)});
    }
    return {};
}

//======================================================================================================================
Result<void> validateTextureView(const Texture& texture, const TextureViewDesc& view) {
    if (auto ok = validateSubresourceRange(texture, view.range); !ok) {
        return ok;
    }
    if (view.format != Format::Unknown && !isSameFormatFamily(view.format, texture.format())) {
        return invalid("TextureViewDesc.format must belong to the texture's format family -- a "
                       "view may only reinterpret the sRGB transfer, not the bit layout");
    }
    return {};
}

//======================================================================================================================
// Integer shifts again, for the same reason maxMipLevels uses them: a mip extent is exact integer
// arithmetic, and the chain floors at one texel rather than at zero.
uint32_t mipExtent(uint32_t base, uint32_t level) {
    const uint32_t extent = base >> level;
    return extent > 0 ? extent : 1;
}

//======================================================================================================================
Result<void> validateBufferRange(const Buffer& buffer, const BufferRange& range) {
    const uint64_t bufferSize = buffer.size();
    if (range.offset >= bufferSize) {
        return std::unexpected(
            Error{ErrorCode::InvalidDesc, "BufferRange.offset " + std::to_string(range.offset) +
                                              " is outside the buffer's " +
                                              std::to_string(bufferSize) + " bytes"});
    }
    const uint64_t size = range.size == kWholeBuffer ? bufferSize - range.offset : range.size;
    return validateBufferBytes(buffer, range.offset, size);
}

//======================================================================================================================
Result<void> validateBufferBytes(const Buffer& buffer, uint64_t offset, uint64_t size) {
    const uint64_t bufferSize = buffer.size();
    if (size == 0) {
        return invalid("a buffer range must cover at least one byte (use kWholeBuffer for the "
                       "whole allocation)");
    }
    if (offset >= bufferSize) {
        return std::unexpected(
            Error{ErrorCode::InvalidDesc, "a buffer range starts at byte " +
                                              std::to_string(offset) + ", outside the buffer's " +
                                              std::to_string(bufferSize) + " bytes"});
    }
    // Compare against the remaining bytes so an enormous size cannot overflow the sum.
    if (size > bufferSize - offset) {
        return std::unexpected(
            Error{ErrorCode::InvalidDesc, "a buffer range covers " + std::to_string(size) +
                                              " bytes from byte " + std::to_string(offset) +
                                              ", past the buffer's " + std::to_string(bufferSize)});
    }
    return {};
}

//======================================================================================================================
Result<void> validateBufferCopy(const Buffer& source, uint64_t sourceOffset,
                                const Buffer& destination, uint64_t destinationOffset,
                                uint64_t size) {
    if (auto ok = validateBufferBytes(source, sourceOffset, size); !ok) {
        return ok;
    }
    if (auto ok = validateBufferBytes(destination, destinationOffset, size); !ok) {
        return ok;
    }
    // Metal leaves an overlapping self-copy undefined rather than defining a direction for it.
    if (&source == &destination && overlaps(sourceOffset, size, destinationOffset, size)) {
        return invalid("copyBuffer: the source and destination ranges of one buffer overlap -- a "
                       "copy has no defined direction, so the result would depend on the hardware");
    }
    return {};
}

//======================================================================================================================
Result<void> validateTextureCopyRegion(const Texture& texture, const TextureCopyRegion& region) {
    if (region.mipLevel >= texture.mipLevels()) {
        return std::unexpected(Error{ErrorCode::InvalidDesc,
                                     "TextureCopyRegion.mipLevel " +
                                         std::to_string(region.mipLevel) +
                                         " is outside the texture's " +
                                         std::to_string(texture.mipLevels()) + " mip level(s)"});
    }
    if (region.arrayLayer >= texture.arrayLayers()) {
        return std::unexpected(
            Error{ErrorCode::InvalidDesc, "TextureCopyRegion.arrayLayer " +
                                              std::to_string(region.arrayLayer) +
                                              " is outside the texture's " +
                                              std::to_string(texture.arrayLayers()) + " layer(s)"});
    }
    if (region.z != 0 || region.depth != 1) {
        return invalid("TextureCopyRegion.z must be 0 and .depth must be 1 -- this RHI models no "
                       "3D texture kind, so there is no third dimension to address");
    }
    if (region.width == 0 || region.height == 0) {
        return invalid("TextureCopyRegion must cover at least one texel in width and height");
    }
    const uint32_t levelWidth = mipExtent(texture.width(), region.mipLevel);
    const uint32_t levelHeight = mipExtent(texture.height(), region.mipLevel);
    if (uint64_t{region.x} + region.width > levelWidth ||
        uint64_t{region.y} + region.height > levelHeight) {
        return std::unexpected(Error{
            ErrorCode::InvalidDesc,
            "TextureCopyRegion covers " + extentOf(region.width, region.height) + " texels from (" +
                std::to_string(region.x) + "," + std::to_string(region.y) + "), past mip level " +
                std::to_string(region.mipLevel) + "'s " + extentOf(levelWidth, levelHeight)});
    }
    return {};
}

//======================================================================================================================
Result<void> validateBufferTextureCopy(const Buffer& buffer, const BufferTextureLayout& layout,
                                       const Texture& texture, const TextureCopyRegion& region) {
    if (auto ok = validateTextureCopyRegion(texture, region); !ok) {
        return ok;
    }
    const uint64_t texelBytes = bytesPerPixel(texture.format());
    if (texelBytes == 0) {
        return invalid("a buffer<->texture copy needs a format with a packed texel size; the "
                       "block-compressed and packed depth formats have none, and no caller needs "
                       "the block arithmetic they would require");
    }
    // Metal addresses both the buffer offset and the row stride in whole texels.
    if (layout.offset % texelBytes != 0 || layout.bytesPerRow % texelBytes != 0) {
        return invalid("BufferTextureLayout.offset and .bytesPerRow must both be multiples of the "
                       "format's texel size");
    }
    const uint64_t rowBytes = uint64_t{region.width} * texelBytes;
    if (layout.bytesPerRow < rowBytes) {
        return std::unexpected(Error{ErrorCode::InvalidDesc,
                                     "BufferTextureLayout.bytesPerRow " +
                                         std::to_string(layout.bytesPerRow) +
                                         " is narrower than the region's " +
                                         std::to_string(rowBytes) + " bytes of texels per row"});
    }
    // A one-slice copy touches no padding after its last row. Compute exactly through the final
    // texel rather than bytesPerRow * height, both to accept that legal footprint and to keep a
    // caller-supplied stride from wrapping the bounds check.
    uint64_t lastRowOffset = 0;
    uint64_t footprint = 0;
    if (!checkedMultiply(layout.bytesPerRow, uint64_t{region.height - 1}, lastRowOffset) ||
        !checkedAdd(lastRowOffset, rowBytes, footprint)) {
        return invalid("BufferTextureLayout row strides overflow the addressable buffer range");
    }
    if (layout.bytesPerSlice != 0 && layout.bytesPerSlice < footprint) {
        return invalid("BufferTextureLayout.bytesPerSlice is smaller than the rows of one slice "
                       "occupy (use 0 for a single-slice copy)");
    }
    return validateBufferBytes(buffer, layout.offset, footprint);
}

//======================================================================================================================
Result<void> validateTextureCopy(const Texture& source, const TextureCopyRegion& sourceRegion,
                                 const Texture& destination,
                                 const TextureCopyRegion& destinationRegion) {
    if (auto ok = validateTextureCopyRegion(source, sourceRegion); !ok) {
        return ok;
    }
    if (auto ok = validateTextureCopyRegion(destination, destinationRegion); !ok) {
        return ok;
    }
    if (sourceRegion.width != destinationRegion.width ||
        sourceRegion.height != destinationRegion.height ||
        sourceRegion.depth != destinationRegion.depth) {
        return std::unexpected(
            Error{ErrorCode::InvalidDesc,
                  "copyTexture: the regions must have the same extent (source " +
                      extentOf(sourceRegion.width, sourceRegion.height) + ", destination " +
                      extentOf(destinationRegion.width, destinationRegion.height) +
                      ") -- a copy neither filters nor rescales"});
    }
    // Reinterpreting one format's bits as another's is what a texture view is for; a copy that did
    // it silently would make the destination's contents depend on the pair of formats involved.
    if (source.format() != destination.format()) {
        return invalid("copyTexture: the source and destination must have the same format");
    }
    if (&source == &destination && sourceRegion.mipLevel == destinationRegion.mipLevel &&
        sourceRegion.arrayLayer == destinationRegion.arrayLayer &&
        overlaps(sourceRegion.x, sourceRegion.width, destinationRegion.x,
                 destinationRegion.width) &&
        overlaps(sourceRegion.y, sourceRegion.height, destinationRegion.y,
                 destinationRegion.height)) {
        return invalid("copyTexture: the source and destination regions of one subresource overlap "
                       "-- a copy has no defined direction, so the result would depend on the "
                       "hardware");
    }
    return {};
}

//======================================================================================================================
Result<void> validateIndirectArgs(const Buffer& buffer, uint64_t offset, uint64_t argsSize) {
    if (offset % kIndirectArgsAlignment != 0) {
        return std::unexpected(
            Error{ErrorCode::InvalidDesc, "an indirect argument offset must be a multiple of " +
                                              std::to_string(kIndirectArgsAlignment) +
                                              " bytes, not " + std::to_string(offset)});
    }
    return validateBufferBytes(buffer, offset, argsSize);
}

//======================================================================================================================
Result<void> validateFrameData(uint32_t slot, const void* data, uint64_t size, uint64_t alignment) {
    if (slot >= CommandList::kMaxBufferBindings) {
        return std::unexpected(Error{
            ErrorCode::InvalidDesc,
            "a frame-data slot must be below the argument table's buffer binding count of " +
                std::to_string(CommandList::kMaxBufferBindings) + ", not " + std::to_string(slot)});
    }
    if (data == nullptr) {
        return invalid("frame data must not be null");
    }
    if (size == 0) {
        return invalid("frame data must not be empty");
    }
    if (alignment < kFrameDataAlignment) {
        return std::unexpected(
            Error{ErrorCode::InvalidDesc, "a frame-data alignment must be at least " +
                                              std::to_string(kFrameDataAlignment) + " bytes, not " +
                                              std::to_string(alignment)});
    }
    if ((alignment & (alignment - 1)) != 0) {
        return std::unexpected(
            Error{ErrorCode::InvalidDesc, "a frame-data alignment must be a power of two, not " +
                                              std::to_string(alignment)});
    }
    // A new page may need as much as alignment - 1 bytes of leading padding before the block.
    // Keep that worst-case capacity calculation representable before the backend reaches Metal.
    if (size > std::numeric_limits<uint64_t>::max() - (alignment - 1)) {
        return std::unexpected(
            Error{ErrorCode::InvalidDesc,
                  "frame-data size plus worst-case alignment padding overflows uint64"});
    }
    return {};
}

//======================================================================================================================
Result<void> validate(const SamplerDesc& desc) {
    if (desc.maxAnisotropy < kMinAnisotropy || desc.maxAnisotropy > kMaxAnisotropy) {
        return invalid("SamplerDesc.maxAnisotropy must be within Metal's anisotropy range of "
                       "1..16");
    }
    return {};
}

//======================================================================================================================
Result<void> validate(const GraphicsPipelineDesc& desc) {
    if (desc.library == nullptr) {
        return invalid("GraphicsPipelineDesc.library must not be null");
    }
    if (desc.vertexEntry.empty()) {
        return invalid("GraphicsPipelineDesc.vertexEntry must not be empty");
    }
    if (desc.fragmentEntry.empty()) {
        return invalid("GraphicsPipelineDesc.fragmentEntry must not be empty");
    }
    // Unknown color format is valid only when a depth attachment gives the pipeline output.
    if (desc.colorFormat == Format::Unknown && desc.depthFormat == Format::Unknown) {
        return invalid("GraphicsPipelineDesc.colorFormat must not be Format::Unknown unless "
                       "depthFormat is set (that pairing is a depth-only pipeline)");
    }
    if (desc.colorFormat != Format::Unknown && !isColorRenderableFormat(desc.colorFormat)) {
        return invalid("GraphicsPipelineDesc.colorFormat must be a color-renderable format "
                       "(an 8-bit unorm, sRGB, RGBA16Float, RG16Float, or R8Unorm one; depth and "
                       "BC1 are not)");
    }
    if (desc.depthFormat != Format::Unknown && !isDepthFormat(desc.depthFormat)) {
        return invalid("GraphicsPipelineDesc.depthFormat must be a depth format (D32Float) or "
                       "Format::Unknown for a depth-less pipeline");
    }
    if ((desc.depthTestEnable || desc.depthWriteEnable) && desc.depthFormat == Format::Unknown) {
        return invalid("GraphicsPipelineDesc.depthFormat must be set when depth test/write is "
                       "enabled");
    }
    if (desc.extraColorCount > kMaxExtraColorTargets) {
        return std::unexpected(
            Error{ErrorCode::InvalidDesc, "GraphicsPipelineDesc.extraColorCount is " +
                                              std::to_string(desc.extraColorCount) + ", past the " +
                                              std::to_string(kMaxExtraColorTargets) +
                                              " extra color attachments a pipeline can declare"});
    }
    // Attachment zero is the primary format's, so an extra with nothing in front of it would
    // number its outputs from one.
    if (desc.extraColorCount > 0 && desc.colorFormat == Format::Unknown) {
        return invalid("GraphicsPipelineDesc.extraColorFormats require colorFormat: extras are "
                       "attachments 1 and up, so a pipeline with no attachment zero cannot have "
                       "them");
    }
    for (uint32_t i = 0; i < desc.extraColorCount; ++i) {
        if (!isColorRenderableFormat(desc.extraColorFormats[i])) {
            return std::unexpected(
                Error{ErrorCode::InvalidDesc,
                      "GraphicsPipelineDesc.extraColorFormats[" + std::to_string(i) +
                          "] must be a color-renderable format; Format::Unknown means no "
                          "attachment, which a counted extra cannot be"});
        }
    }
    return {};
}

//======================================================================================================================
Result<void> validate(const ComputePipelineDesc& desc) {
    if (desc.library == nullptr) {
        return invalid("ComputePipelineDesc.library must not be null");
    }
    if (desc.computeEntry.empty()) {
        return invalid("ComputePipelineDesc.computeEntry must not be empty");
    }
    // A zero component would dispatch a grid of nothing while the kernel still declared threads.
    for (const uint32_t threads : desc.threadsPerThreadgroup) {
        if (threads == 0) {
            return invalid("ComputePipelineDesc.threadsPerThreadgroup components must all be at "
                           "least 1 (an unused dimension is 1, not 0)");
        }
    }
    return {};
}

//======================================================================================================================
Result<void> validate(const SwapchainDesc& desc) {
    if (desc.nativeLayer == nullptr) {
        return invalid("SwapchainDesc.nativeLayer must not be null");
    }
    if (desc.width == 0) {
        return invalid("SwapchainDesc.width must be greater than zero");
    }
    if (desc.height == 0) {
        return invalid("SwapchainDesc.height must be greater than zero");
    }
    if (desc.format == Format::Unknown) {
        return invalid("SwapchainDesc.format must not be Format::Unknown");
    }
    if (!isSwapchainFormat(desc.format)) {
        return invalid(
            "SwapchainDesc.format must be an SDR color-renderable 8-bit unorm or sRGB format");
    }
    return {};
}

//======================================================================================================================
Result<void> validate(const HeapDesc& desc) {
    if (desc.size == 0) {
        return invalid("HeapDesc.size must be greater than zero");
    }
    return {};
}

//======================================================================================================================
// A zero footprint is checked as well as the bounds: it means the backend declined to size the
// descriptor, and placing at an alignment of zero would divide by it below.
Result<void> validatePlacement(const Heap& heap, uint64_t offset, const SizeAlign& footprint) {
    if (footprint.size == 0 || footprint.alignment == 0) {
        return invalid("a placed resource must have a non-zero size and alignment; this descriptor "
                       "has none, so it cannot be positioned in a heap");
    }
    if (offset % footprint.alignment != 0) {
        return std::unexpected(
            Error{ErrorCode::InvalidDesc,
                  "placement offset " + std::to_string(offset) + " is not a multiple of the " +
                      std::to_string(footprint.alignment) + "-byte alignment this resource needs"});
    }
    // Subtraction rather than offset + size so a caller-supplied offset near the top of the range
    // cannot wrap past the comparison.
    if (offset > heap.size() || footprint.size > heap.size() - offset) {
        return std::unexpected(
            Error{ErrorCode::InvalidDesc, "a placement of " + std::to_string(footprint.size) +
                                              " bytes at offset " + std::to_string(offset) +
                                              " runs past the end of a " +
                                              std::to_string(heap.size()) + "-byte heap"});
    }
    return {};
}

//======================================================================================================================
Result<void> validateRenderPassTargets(const Texture* color, const Texture* depth) {
    if (color == nullptr && depth == nullptr) {
        return invalid("RenderPassDesc: a pass needs at least one attachment -- set colorTarget, "
                       "depthTarget, or both");
    }
    if (color != nullptr && depth != nullptr &&
        (color->width() != depth->width() || color->height() != depth->height())) {
        // Metal silently clips mismatched attachments, so include both extents in the error.
        return std::unexpected(
            Error{ErrorCode::InvalidDesc,
                  "RenderPassDesc: colorTarget and depthTarget must have the same extent (color " +
                      extentOf(*color) + ", depth " + extentOf(*depth) + ")"});
    }
    return {};
}

//======================================================================================================================
Result<void> validateExtraColorTargets(const Texture* color, const ExtraColorTarget* extraColor,
                                       uint32_t extraColorCount) {
    if (extraColorCount > kMaxExtraColorTargets) {
        return std::unexpected(
            Error{ErrorCode::InvalidDesc, "RenderPassDesc.extraColorCount is " +
                                              std::to_string(extraColorCount) + ", past the " +
                                              std::to_string(kMaxExtraColorTargets) +
                                              " extra color attachments a pass can declare"});
    }
    if (extraColorCount == 0) {
        return {};
    }
    // Attachment zero is the primary target's, so an extra with nothing in front of it would be a
    // pass whose attachments start at index one.
    if (color == nullptr) {
        return invalid("RenderPassDesc.extraColor requires colorTarget: extras are attachments 1 "
                       "and up, so a pass with no attachment zero cannot have them");
    }
    for (uint32_t i = 0; i < extraColorCount; ++i) {
        const std::string name = "RenderPassDesc.extraColor[" + std::to_string(i) + "]";
        const Texture* target = extraColor[i].target;
        if (target == nullptr) {
            return std::unexpected(
                Error{ErrorCode::InvalidDesc,
                      name + ".target must not be null; entries past extraColorCount are the "
                             "unused ones"});
        }
        if (!isColorRenderableFormat(target->format())) {
            return std::unexpected(Error{ErrorCode::InvalidDesc,
                                         name + ".target must have a color-renderable format"});
        }
        if (target->width() != color->width() || target->height() != color->height()) {
            // Metal silently clips mismatched attachments, so include both extents in the error.
            return std::unexpected(Error{ErrorCode::InvalidDesc,
                                         name + ".target must have colorTarget's extent (color " +
                                             extentOf(*color) + ", extra " + extentOf(*target) +
                                             ")"});
        }
    }
    return {};
}

} // namespace lmx::rhi
