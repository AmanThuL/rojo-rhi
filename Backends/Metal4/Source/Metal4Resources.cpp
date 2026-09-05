//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Resources.cpp
/// @brief Implements Metal 4 buffer, texture, sampler, shader-library, and pipeline wrappers.
//----------------------------------------------------------------------------------------------------------------------
#include "Metal4Resources.h"

#include "Core/Assert.h"
#include "RHI/CaptureSchema.h"

#include <cstring>
#include <string>
#include <utility>

namespace lmx::rhi::metal4 {

//======================================================================================================================
ResidencyRegistration::ResidencyRegistration(NS::SharedPtr<MTL::ResidencySet> residency,
                                             const MTL::Allocation* allocation)
    : m_residency(std::move(residency)), m_allocation(allocation) {
    if (!m_residency) {
        return;
    }
    LMX_ASSERT(m_allocation != nullptr, "ResidencyRegistration: allocation must not be null");
    // Metal 4 requires explicit residency; commit republishes the set after each membership change.
    m_residency->addAllocation(m_allocation);
    m_residency->commit();
}

//======================================================================================================================
ResidencyRegistration::~ResidencyRegistration() {
    reset();
}

//======================================================================================================================
void ResidencyRegistration::reset() {
    if (!m_residency) {
        return;
    }
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    m_residency->removeAllocation(m_allocation);
    m_residency->commit();
    // Release the set here, inside this pool, rather than leaving it to the member's own
    // destruction after the owning wrapper's body has returned; nulling it is also what makes a
    // second call -- the destructor's, after an owner already reset by hand -- a no-op.
    m_residency.reset();
    m_allocation = nullptr;
}

//======================================================================================================================
// Resource wrappers unregister the same native pointer used as their capture identity. Transient
// drawable wrappers were never registered, so their removal is a no-op.
//
// The explicit release order below is the declaration order's reverse, performed by hand so that
// all of it happens inside this pool: residency must drop the allocation while the allocation is
// still alive, and the Metal object must deallocate with a pool in place.
Metal4Buffer::~Metal4Buffer() {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    debug::CaptureSchema::instance().unregisterResource(m_buffer.get());
    m_residency.reset();
    m_buffer.reset();
}

//======================================================================================================================
Metal4Heap::~Metal4Heap() {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    m_residency.reset();
    m_heap.reset();
}

//======================================================================================================================
void Metal4Buffer::readback(void* out, uint64_t outSize) {
    LMX_ASSERT(out != nullptr, "Buffer::readback: destination must not be null");
    LMX_ASSERT(m_cpuReadback,
               "Buffer::readback: buffer was not created with BufferDesc.cpuReadback");
    LMX_ASSERT(outSize <= m_buffer->length(),
               "Buffer::readback: outSize reads past the end of the buffer");
    // Shared storage makes the allocation itself CPU-visible, so the readback is the copy out --
    // there is nothing to resolve or untile first.
    std::memcpy(out, m_buffer->contents(), outSize);
}

//======================================================================================================================
// Same explicit order as ~Metal4Buffer, with the cached views released before the texture they
// are views of.
Metal4Texture::~Metal4Texture() {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    debug::CaptureSchema::instance().unregisterResource(m_texture.get());
    m_residency.reset();
    m_views.clear();
    m_texture.reset();
}

//======================================================================================================================
void Metal4Texture::readback(void* out, uint64_t outSize) {
    LMX_ASSERT(out != nullptr, "Texture::readback: destination must not be null");
    LMX_ASSERT(m_info.readbackBytesPerPixel > 0,
               "Texture::readback: texture was not created with TextureDesc.cpuReadback");
    // TextureDesc validation already refused every format without a packed texel size, so the
    // destination and the source rows share this one stride.
    const uint64_t bytesPerRow = uint64_t{m_info.width} * m_info.readbackBytesPerPixel;
    const uint64_t expected = bytesPerRow * m_info.height;
    LMX_ASSERT(outSize == expected,
               "Texture::readback: outSize must be width*height*bytesPerPixel(format)");

    const MTL::Region region = MTL::Region::Make2D(0, 0, m_info.width, m_info.height);
    m_texture->getBytes(out, bytesPerRow, region, 0);
}

//======================================================================================================================
MTL::Texture* Metal4Texture::viewFor(const TextureViewDesc& desc) {
    const uint32_t baseMip = desc.range.baseMipLevel;
    const uint32_t mipCount = desc.range.mipLevelCount == kAllMipLevels ? m_info.mipLevels - baseMip
                                                                        : desc.range.mipLevelCount;
    const uint32_t baseLayer = desc.range.baseArrayLayer;
    const uint32_t layerCount = desc.range.arrayLayerCount == kAllArrayLayers
                                    ? m_info.arrayLayers - baseLayer
                                    : desc.range.arrayLayerCount;
    const MTL::PixelFormat format =
        desc.format == Format::Unknown ? m_texture->pixelFormat() : toMTL(desc.format);

    // The whole texture in its own format is the texture: a view of it would be an allocation and
    // a cache entry that resolve to the same bits.
    if (format == m_texture->pixelFormat() && baseMip == 0 && mipCount == m_info.mipLevels &&
        baseLayer == 0 && layerCount == m_info.arrayLayers) {
        return m_texture.get();
    }

    for (const View& cached : m_views) {
        if (cached.baseMipLevel == baseMip && cached.mipLevelCount == mipCount &&
            cached.baseArrayLayer == baseLayer && cached.arrayLayerCount == layerCount &&
            cached.format == format) {
            return cached.texture.get();
        }
    }

    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    MTL::TextureType viewType = m_texture->textureType();
    if (m_texture->textureType() == MTL::TextureTypeCube &&
        (baseLayer != 0 || layerCount != m_info.arrayLayers)) {
        // Metal explicitly permits Cube -> 2D/2DArray views. One face is the Texture2D shape shader
        // code normally asks for; a larger non-cube subset preserves its layers as a 2D array.
        viewType = layerCount == 1 ? MTL::TextureType2D : MTL::TextureType2DArray;
    }

    NS::SharedPtr<MTL::Texture> view = NS::TransferPtr(
        m_texture->newTextureView(format, viewType, NS::Range::Make(baseMip, mipCount),
                                  NS::Range::Make(baseLayer, layerCount)));
    LMX_ASSERT(view, "TextureViewDesc: Metal refused to create the requested texture view");
    const NS::String* parentLabel = m_texture->label();
    const char* utf8 = parentLabel != nullptr ? parentLabel->utf8String() : nullptr;
    view->setLabel(makeString(std::string(utf8 != nullptr ? utf8 : "lmx.texture.unnamed") +
                              ".view.mip" + std::to_string(baseMip) + "+" +
                              std::to_string(mipCount))
                       .get());

    m_views.push_back({.baseMipLevel = baseMip,
                       .mipLevelCount = mipCount,
                       .baseArrayLayer = baseLayer,
                       .arrayLayerCount = layerCount,
                       .format = format,
                       .texture = std::move(view)});
    return m_views.back().texture.get();
}

} // namespace lmx::rhi::metal4
