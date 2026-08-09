#include "RHI/Metal4/Metal4Resources.h"

#include "Core/Assert.h"
#include "RHI/CaptureSchema.h"

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
    if (!m_residency) {
        return;
    }
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    m_residency->removeAllocation(m_allocation);
    m_residency->commit();
}

//======================================================================================================================
// Resource wrappers unregister the same native pointer used as their capture identity. Transient
// drawable wrappers were never registered, so their removal is a no-op.
Metal4Buffer::~Metal4Buffer() {
    debug::CaptureSchema::instance().unregisterResource(m_buffer.get());
}

//======================================================================================================================
Metal4Texture::~Metal4Texture() {
    debug::CaptureSchema::instance().unregisterResource(m_texture.get());
}

//======================================================================================================================
void Metal4Texture::readback(void* out, uint64_t outSize) {
    LMX_ASSERT(out != nullptr, "Texture::readback: destination must not be null");
    LMX_ASSERT(m_cpuReadback,
               "Texture::readback: texture was not created with TextureDesc.cpuReadback");
    // Readback validation limits this path to tightly packed four-byte formats.
    const uint64_t expected = uint64_t{m_width} * m_height * 4;
    LMX_ASSERT(outSize == expected, "Texture::readback: outSize must be width*height*4");

    const MTL::Region region = MTL::Region::Make2D(0, 0, m_width, m_height);
    m_texture->getBytes(out, m_width * 4, region, 0);
}

} // namespace lmx::rhi::metal4
