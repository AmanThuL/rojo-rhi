#include "RHI/Validate.h"

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
// on write; block compression cannot run per fragment. RG16Float is renderable on the hardware but
// stays out until a pass actually renders into one.
bool isColorRenderableFormat(Format format) {
    return format == Format::BGRA8Unorm || format == Format::RGBA8Unorm ||
           format == Format::RGBA8Unorm_sRGB || format == Format::RGBA16Float;
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
std::string extentOf(const Texture& texture) {
    return std::to_string(texture.width()) + "x" + std::to_string(texture.height());
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
    case Format::RGBA16Float:
        return 8;
    // A BC1 block covers 4x4 texels, so "bytes per pixel" is not expressible; D32Float and
    // RG16Float are packed formats with no readback caller.
    case Format::BC1Unorm:
    case Format::BC1Unorm_sRGB:
    case Format::D32Float:
    case Format::RG16Float:
    case Format::Unknown:
        break;
    }
    return 0;
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
    if (!desc.renderTarget && !desc.sampled && !desc.cpuReadback) {
        return invalid("TextureDesc has no usage: set at least one of renderTarget, sampled, or "
                       "cpuReadback");
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
                       "(an 8-bit unorm, sRGB, or RGBA16Float one; depth, RG16Float and BC1 are "
                       "not)");
    }
    if (desc.depthFormat != Format::Unknown && !isDepthFormat(desc.depthFormat)) {
        return invalid("GraphicsPipelineDesc.depthFormat must be a depth format (D32Float) or "
                       "Format::Unknown for a depth-less pipeline");
    }
    if ((desc.depthTestEnable || desc.depthWriteEnable) && desc.depthFormat == Format::Unknown) {
        return invalid("GraphicsPipelineDesc.depthFormat must be set when depth test/write is "
                       "enabled");
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

} // namespace lmx::rhi
