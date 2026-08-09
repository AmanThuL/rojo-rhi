#include "RHI/Metal4/Metal4Device.h"

#include "Core/Assert.h"
#include "Core/Log.h"
#include "RHI/CaptureSchema.h"
#include "RHI/Metal4/Metal4DevicePrivate.h"
#include "RHI/Metal4/Metal4Resources.h"
#include "RHI/Metal4/Metal4Swapchain.h"
#include "RHI/Validate.h"

#include <cstring>
#include <utility>

namespace lmx::rhi::metal4 {
namespace {

using device_detail::describe;
using device_detail::fail;
using device_detail::labelOrFallback;
using device_detail::resolveLabel;

constexpr uint32_t kCubeFaceCount = 6;

//======================================================================================================================
NS::UInteger mipExtent(uint32_t base, uint32_t level) {
    const uint32_t extent = base >> level;
    return extent > 0 ? extent : 1;
}

//======================================================================================================================
MTL::SamplerMinMagFilter toMTLMinMag(FilterMode filter) {
    return filter == FilterMode::Linear ? MTL::SamplerMinMagFilterLinear
                                        : MTL::SamplerMinMagFilterNearest;
}

//======================================================================================================================
MTL::SamplerMipFilter toMTLMip(FilterMode filter) {
    return filter == FilterMode::Linear ? MTL::SamplerMipFilterLinear
                                        : MTL::SamplerMipFilterNearest;
}

//======================================================================================================================
MTL::SamplerAddressMode toMTL(AddressMode mode) {
    return mode == AddressMode::Wrap ? MTL::SamplerAddressModeRepeat
                                     : MTL::SamplerAddressModeClampToEdge;
}

//======================================================================================================================
// CompareFunc::Never selects an ordinary sampler; the ordering functions select a comparison one.
MTL::CompareFunction toMTL(CompareFunc compare) {
    switch (compare) {
    case CompareFunc::Never:
        return MTL::CompareFunctionNever;
    case CompareFunc::LessEqual:
        return MTL::CompareFunctionLessEqual;
    case CompareFunc::GreaterEqual:
        return MTL::CompareFunctionGreaterEqual;
    }
    return MTL::CompareFunctionNever;
}

} // namespace

//======================================================================================================================
Result<std::unique_ptr<Swapchain>> Metal4Device::createSwapchain(const SwapchainDesc& desc) {
    if (auto ok = validate(desc); !ok) {
        return std::unexpected(ok.error());
    }
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    // Retain the window-owned layer for the swapchain lifetime.
    NS::SharedPtr<CA::MetalLayer> layer =
        NS::RetainPtr(static_cast<CA::MetalLayer*>(desc.nativeLayer));

    layer->setDevice(m_device.get());
    layer->setPixelFormat(toMTL(desc.format));
    layer->setDrawableSize(
        CGSize{static_cast<CGFloat>(desc.width), static_cast<CGFloat>(desc.height)});
    // Drawables are never sampled or read back, allowing Core Animation's cheapest layout.
    layer->setFramebufferOnly(true);

    NS::SharedPtr<MTL::ResidencySet> layerResidency;
    if (MTL::ResidencySet* set = layer->residencySet(); set != nullptr) {
        layerResidency = NS::RetainPtr(set);
    } else {
        LMX_LOG_WARN("CAMetalLayer vends no residency set; relying on Metal's default drawable "
                     "residency handling");
    }

    return std::make_unique<Metal4Swapchain>(std::move(layer), m_queue, std::move(layerResidency));
}

//======================================================================================================================
Result<std::unique_ptr<Buffer>> Metal4Device::createBuffer(const BufferDesc& desc,
                                                           const void* initialData) {
    if (auto ok = validate(desc); !ok) {
        return std::unexpected(ok.error());
    }
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    // Unified memory keeps shared buffers GPU-visible while supporting CPU uploads and updates.
    NS::SharedPtr<MTL::Buffer> buffer =
        NS::TransferPtr(m_device->newBuffer(desc.size, MTL::ResourceStorageModeShared));
    if (!buffer) {
        return fail(ErrorCode::ResourceCreationFailed,
                    "failed to create buffer of " + std::to_string(desc.size) + " bytes");
    }
    if (initialData != nullptr) {
        std::memcpy(buffer->contents(), initialData, desc.size);
    }
    const std::string_view label = resolveLabel(desc.label, "lmx.buffer.unnamed");
    buffer->setLabel(makeString(label).get());

    // Capture identity uses the same native pointer the wrapper unregisters at destruction.
    debug::CaptureSchema::instance().registerBuffer(buffer.get(), label, desc.size);

    // ResidencyRegistration removes the allocation when the wrapper dies.
    return std::make_unique<Metal4Buffer>(std::move(buffer), m_residency);
}

//======================================================================================================================
Result<std::unique_ptr<Texture>> Metal4Device::createTexture(const TextureDesc& desc,
                                                             std::span<const TextureMip> mips) {
    if (auto ok = validate(desc); !ok) {
        return std::unexpected(ok.error());
    }
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    const uint32_t faceCount = desc.kind == TextureKind::Cube ? kCubeFaceCount : 1;
    LMX_ASSERT(mips.empty() || mips.size() == size_t{desc.mipLevels} * faceCount,
               "createTexture: mips must be empty or hold mipLevels * faceCount entries");

    auto textureDesc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
    // For TypeCube, array length counts cubes rather than faces and remains one.
    textureDesc->setTextureType(desc.kind == TextureKind::Cube ? MTL::TextureTypeCube
                                                               : MTL::TextureType2D);
    textureDesc->setPixelFormat(toMTL(desc.format));
    textureDesc->setWidth(desc.width);
    textureDesc->setHeight(desc.height);
    textureDesc->setMipmapLevelCount(desc.mipLevels);
    MTL::TextureUsage usage =
        desc.sampled || desc.cpuReadback ? MTL::TextureUsageShaderRead : MTL::TextureUsageUnknown;
    if (desc.renderTarget) {
        usage |= MTL::TextureUsageRenderTarget;
    }
    LMX_ASSERT(usage != MTL::TextureUsageUnknown,
               "TextureDesc: a texture that is neither renderTarget, sampled, nor cpuReadback "
               "has no reachable use");
    textureDesc->setUsage(usage);
    // CPU upload/readback requires Shared storage; otherwise retain the driver's Private-layout
    // freedom on unified memory.
    textureDesc->setStorageMode(desc.cpuReadback || !mips.empty() ? MTL::StorageModeShared
                                                                  : MTL::StorageModePrivate);

    NS::SharedPtr<MTL::Texture> texture = NS::TransferPtr(m_device->newTexture(textureDesc.get()));
    if (!texture) {
        return fail(ErrorCode::ResourceCreationFailed,
                    "failed to create " + std::to_string(desc.width) + "x" +
                        std::to_string(desc.height) + " texture");
    }
    const std::string_view label = resolveLabel(desc.label, "lmx.texture.unnamed");
    texture->setLabel(makeString(label).get());

    // Capture identity follows the native pointer owned by the wrapper.
    debug::CaptureSchema::instance().registerTexture(texture.get(), label, desc);

    // The flat span stores all mip levels for each face before advancing to the next face.
    for (size_t index = 0; index < mips.size(); ++index) {
        const TextureMip& mip = mips[index];
        // Null entries leave levels available for later mip generation.
        if (mip.data == nullptr) {
            continue;
        }
        LMX_ASSERT(mip.bytesPerRow > 0,
                   "createTexture: a TextureMip with data must state its bytesPerRow");
        const NS::UInteger face = index / desc.mipLevels;
        const uint32_t level = static_cast<uint32_t>(index % desc.mipLevels);
        // Full-level writes preserve block alignment for compressed formats.
        const MTL::Region region =
            MTL::Region::Make2D(0, 0, mipExtent(desc.width, level), mipExtent(desc.height, level));
        // A single slice has no image-to-image stride.
        texture->replaceRegion(region, level, face, mip.data, mip.bytesPerRow,
                               /*bytesPerImage=*/0);
    }

    return std::make_unique<Metal4Texture>(std::move(texture), desc.width, desc.height,
                                           desc.cpuReadback ? bytesPerPixel(desc.format) : 0,
                                           m_residency);
}

//======================================================================================================================
Result<std::unique_ptr<Sampler>> Metal4Device::createSampler(const SamplerDesc& desc) {
    if (auto ok = validate(desc); !ok) {
        return std::unexpected(ok.error());
    }
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    auto samplerDesc = NS::TransferPtr(MTL::SamplerDescriptor::alloc()->init());
    samplerDesc->setMinFilter(toMTLMinMag(desc.filter));
    samplerDesc->setMagFilter(toMTLMinMag(desc.filter));
    samplerDesc->setMipFilter(toMTLMip(desc.filter));
    samplerDesc->setSAddressMode(toMTL(desc.addressMode));
    samplerDesc->setTAddressMode(toMTL(desc.addressMode));
    samplerDesc->setRAddressMode(toMTL(desc.addressMode));
    samplerDesc->setMaxAnisotropy(desc.maxAnisotropy);
    samplerDesc->setCompareFunction(toMTL(desc.compare));
    // gpuResourceID is valid only for samplers created with argument-buffer support.
    samplerDesc->setSupportArgumentBuffers(true);
    samplerDesc->setLabel(labelOrFallback(desc.label, "lmx.sampler.unnamed").get());

    NS::SharedPtr<MTL::SamplerState> sampler =
        NS::TransferPtr(m_device->newSamplerState(samplerDesc.get()));
    // newSamplerState exposes no error object beyond a null result.
    if (!sampler) {
        return fail(ErrorCode::ResourceCreationFailed, "failed to create sampler state");
    }

    return std::make_unique<Metal4Sampler>(std::move(sampler));
}

} // namespace lmx::rhi::metal4
