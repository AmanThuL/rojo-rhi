//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4DeviceResources.cpp
/// @brief Implements Metal 4 resource and sampler creation for the device.
//----------------------------------------------------------------------------------------------------------------------
#include "Metal4Device.h"

#include "Core/Assert.h"
#include "Core/Log.h"
#include "Metal4DevicePrivate.h"
#include "Metal4Resources.h"
#include "Metal4Swapchain.h"
#include "RHI/CaptureSchema.h"
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

// What every placed resource is allocated as. Private because a placement heap holds one storage
// mode for everything in it and nothing placed is ever CPU-visible here; Untracked because the
// whole point of placement is that the caller states the ordering -- Metal's own tracking cannot
// see that two resources share bytes, so leaving it on would cost per-encoder bookkeeping and still
// not order an alias.
constexpr MTL::ResourceOptions kPlacedResourceOptions =
    MTL::ResourceStorageModePrivate | MTL::ResourceHazardTrackingModeUntracked;

//======================================================================================================================
// The Metal descriptor a TextureDesc compiles to, shared by ordinary creation, placed creation, and
// the size query -- so a texture is sized against exactly the descriptor it will be created from.
//
// `hasInitialData` selects Shared storage for the upload path; `placed` overrides both storage and
// hazard tracking with what a placement heap requires.
NS::SharedPtr<MTL::TextureDescriptor> makeTextureDescriptor(const TextureDesc& desc,
                                                            bool hasInitialData, bool placed) {
    auto textureDesc = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
    // For TypeCube, array length counts cubes rather than faces and remains one.
    textureDesc->setTextureType(desc.kind == TextureKind::Cube ? MTL::TextureTypeCube
                                                               : MTL::TextureType2D);
    textureDesc->setPixelFormat(toMTL(desc.format));
    textureDesc->setWidth(desc.width);
    textureDesc->setHeight(desc.height);
    textureDesc->setMipmapLevelCount(desc.mipLevels);
    MTL::TextureUsage usage = desc.sampled || desc.cpuReadback || desc.storageRead
                                  ? MTL::TextureUsageShaderRead
                                  : MTL::TextureUsageUnknown;
    if (desc.renderTarget) {
        usage |= MTL::TextureUsageRenderTarget;
    }
    if (desc.storageWrite) {
        usage |= MTL::TextureUsageShaderWrite;
    }
    // Metal requires the parent to opt into subresource and format views at creation. Sampled
    // textures (including the cpuReadback path, which also grants ShaderRead) need it for
    // sRGB/linear reinterpretation and cube-face views; storage textures need it for per-mip binds.
    if (desc.sampled || desc.storageRead || desc.storageWrite || desc.cpuReadback) {
        usage |= MTL::TextureUsagePixelFormatView;
    }
    LMX_ASSERT(usage != MTL::TextureUsageUnknown,
               "TextureDesc: a texture with no renderTarget, sampled, storage, or cpuReadback "
               "usage has no reachable use");
    textureDesc->setUsage(usage);
    if (placed) {
        textureDesc->setStorageMode(MTL::StorageModePrivate);
        textureDesc->setHazardTrackingMode(MTL::HazardTrackingModeUntracked);
        return textureDesc;
    }
    // CPU upload/readback requires Shared storage; otherwise retain the driver's Private-layout
    // freedom on unified memory.
    textureDesc->setStorageMode(desc.cpuReadback || hasInitialData ? MTL::StorageModeShared
                                                                   : MTL::StorageModePrivate);
    return textureDesc;
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

    return std::make_unique<Metal4Swapchain>(std::move(layer), desc.format, m_queue,
                                             std::move(layerResidency));
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
    return std::make_unique<Metal4Buffer>(std::move(buffer), desc, m_residency);
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

    NS::SharedPtr<MTL::TextureDescriptor> textureDesc =
        makeTextureDescriptor(desc, !mips.empty(), /*placed=*/false);

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

    const Metal4TextureInfo info{.format = desc.format,
                                 .width = desc.width,
                                 .height = desc.height,
                                 .mipLevels = desc.mipLevels,
                                 .arrayLayers = faceCount,
                                 .readbackBytesPerPixel =
                                     desc.cpuReadback ? bytesPerPixel(desc.format) : 0};
    return std::make_unique<Metal4Texture>(std::move(texture), info, m_residency);
}

//======================================================================================================================
Result<std::unique_ptr<Heap>> Metal4Device::createHeap(const HeapDesc& desc) {
    if (auto ok = validate(desc); !ok) {
        return std::unexpected(ok.error());
    }
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    auto heapDesc = NS::TransferPtr(MTL::HeapDescriptor::alloc()->init());
    // Placement, not the sub-allocating automatic type: the caller chooses every offset, which is
    // what lets two resources deliberately share bytes.
    heapDesc->setType(MTL::HeapTypePlacement);
    heapDesc->setStorageMode(MTL::StorageModePrivate);
    heapDesc->setHazardTrackingMode(MTL::HazardTrackingModeUntracked);
    heapDesc->setSize(desc.size);

    NS::SharedPtr<MTL::Heap> heap = NS::TransferPtr(m_device->newHeap(heapDesc.get()));
    if (!heap) {
        return fail(ErrorCode::ResourceCreationFailed,
                    "failed to create a placement heap of " + std::to_string(desc.size) + " bytes");
    }
    heap->setLabel(labelOrFallback(desc.label, "lmx.heap.unnamed").get());

    return std::make_unique<Metal4Heap>(std::move(heap), m_residency);
}

//======================================================================================================================
Result<std::unique_ptr<Texture>> Metal4Device::createPlacedTexture(Heap& heap, uint64_t offset,
                                                                   const TextureDesc& desc) {
    if (auto ok = validate(desc); !ok) {
        return std::unexpected(ok.error());
    }
    // Placement heaps here are device-private, so there is no CPU-visible allocation to read back
    // through and no upload span the caller could have passed.
    if (desc.cpuReadback) {
        return fail(ErrorCode::InvalidDesc,
                    "createPlacedTexture: a placed texture is device-private, so TextureDesc."
                    "cpuReadback cannot be honoured");
    }
    if (auto ok = validatePlacement(heap, offset, textureSizeAlign(desc)); !ok) {
        return std::unexpected(ok.error());
    }
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    NS::SharedPtr<MTL::TextureDescriptor> textureDesc =
        makeTextureDescriptor(desc, /*hasInitialData=*/false, /*placed=*/true);
    NS::SharedPtr<MTL::Texture> texture = NS::TransferPtr(
        static_cast<Metal4Heap&>(heap).handle()->newTexture(textureDesc.get(), offset));
    if (!texture) {
        return fail(ErrorCode::ResourceCreationFailed,
                    "failed to place a " + std::to_string(desc.width) + "x" +
                        std::to_string(desc.height) + " texture at heap offset " +
                        std::to_string(offset));
    }
    const std::string_view label = resolveLabel(desc.label, "lmx.texture.placed.unnamed");
    texture->setLabel(makeString(label).get());
    debug::CaptureSchema::instance().registerTexture(texture.get(), label, desc);

    const Metal4TextureInfo info{.format = desc.format,
                                 .width = desc.width,
                                 .height = desc.height,
                                 .mipLevels = desc.mipLevels,
                                 .arrayLayers = desc.kind == TextureKind::Cube ? kCubeFaceCount : 1,
                                 .readbackBytesPerPixel = 0};
    // Null residency set: the heap this sits in is what the residency set holds, and it covers
    // everything placed inside it.
    return std::make_unique<Metal4Texture>(std::move(texture), info,
                                           NS::SharedPtr<MTL::ResidencySet>{});
}

//======================================================================================================================
Result<std::unique_ptr<Buffer>> Metal4Device::createPlacedBuffer(Heap& heap, uint64_t offset,
                                                                 const BufferDesc& desc) {
    if (auto ok = validate(desc); !ok) {
        return std::unexpected(ok.error());
    }
    if (desc.cpuReadback) {
        return fail(ErrorCode::InvalidDesc,
                    "createPlacedBuffer: a placed buffer is device-private, so BufferDesc."
                    "cpuReadback cannot be honoured");
    }
    if (auto ok = validatePlacement(heap, offset, bufferSizeAlign(desc)); !ok) {
        return std::unexpected(ok.error());
    }
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    NS::SharedPtr<MTL::Buffer> buffer =
        NS::TransferPtr(static_cast<Metal4Heap&>(heap).handle()->newBuffer(
            desc.size, kPlacedResourceOptions, offset));
    if (!buffer) {
        return fail(ErrorCode::ResourceCreationFailed,
                    "failed to place a " + std::to_string(desc.size) +
                        "-byte buffer at heap offset " + std::to_string(offset));
    }
    const std::string_view label = resolveLabel(desc.label, "lmx.buffer.placed.unnamed");
    buffer->setLabel(makeString(label).get());
    debug::CaptureSchema::instance().registerBuffer(buffer.get(), label, desc.size);

    // Null residency set, for the reason given in createPlacedTexture.
    return std::make_unique<Metal4Buffer>(std::move(buffer), desc,
                                          NS::SharedPtr<MTL::ResidencySet>{});
}

//======================================================================================================================
SizeAlign Metal4Device::textureSizeAlign(const TextureDesc& desc) const {
    const Result<void> descOk = validate(desc);
    LMX_ASSERT(descOk.has_value(), descOk.error().message);
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    const MTL::SizeAndAlign sizeAlign = m_device->heapTextureSizeAndAlign(
        makeTextureDescriptor(desc, /*hasInitialData=*/false, /*placed=*/true).get());
    return {.size = sizeAlign.size, .alignment = sizeAlign.align};
}

//======================================================================================================================
SizeAlign Metal4Device::bufferSizeAlign(const BufferDesc& desc) const {
    const Result<void> descOk = validate(desc);
    LMX_ASSERT(descOk.has_value(), descOk.error().message);
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    const MTL::SizeAndAlign sizeAlign =
        m_device->heapBufferSizeAndAlign(desc.size, kPlacedResourceOptions);
    return {.size = sizeAlign.size, .alignment = sizeAlign.align};
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
