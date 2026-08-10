//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Swapchain.cpp
/// @brief Implements drawable acquisition, presentation, resizing, and teardown for Metal 4.
//----------------------------------------------------------------------------------------------------------------------
#include "Metal4Swapchain.h"

#include "Core/Assert.h"
#include "Core/Log.h"

#include <string>
#include <utility>

namespace lmx::rhi::metal4 {

//======================================================================================================================
Metal4Swapchain::Metal4Swapchain(NS::SharedPtr<CA::MetalLayer> layer,
                                 NS::SharedPtr<MTL4::CommandQueue> queue,
                                 NS::SharedPtr<MTL::ResidencySet> layerResidency)
    : m_layer(std::move(layer)), m_queue(std::move(queue)),
      m_layerResidency(std::move(layerResidency)) {
    // Attach the layer-owned drawable residency set for the swapchain lifetime.
    if (m_layerResidency) {
        m_queue->addResidencySet(m_layerResidency.get());
    }
}

//======================================================================================================================
Metal4Swapchain::~Metal4Swapchain() {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    drainQueue(m_queue.get());

    if (m_layerResidency) {
        m_queue->removeResidencySet(m_layerResidency.get());
    }
    // Release an acquired but unpresented drawable back to the layer during teardown.
    m_texture.reset();
    m_drawable.reset();
}

//======================================================================================================================
Result<Texture*> Metal4Swapchain::acquireNextTexture() {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    LMX_ASSERT(!m_drawable, "acquireNextTexture: the previous drawable has not been presented "
                            "yet -- call Device::endFrame(swapchain) first");

    // Drawable exhaustion or timeout is recoverable; the caller can skip this frame.
    CA::MetalDrawable* drawable = m_layer->nextDrawable();
    if (drawable == nullptr) {
        return std::unexpected(
            Error{ErrorCode::SwapchainFailed, "no drawable available from CAMetalLayer"});
    }
    MTL::Texture* texture = drawable->texture();
    if (texture == nullptr) {
        return std::unexpected(
            Error{ErrorCode::SwapchainFailed, "CAMetalLayer drawable has no texture"});
    }

    m_drawable = NS::RetainPtr(drawable);
    // Use the acquired texture's extent because resize may leave one old-sized drawable queued.
    // Layer-owned drawables use the layer residency set instead of per-frame device registration.
    m_texture = std::make_unique<Metal4Texture>(
        NS::RetainPtr(texture), static_cast<uint32_t>(texture->width()),
        static_cast<uint32_t>(texture->height()), /*readbackBytesPerPixel=*/0,
        /*residency=*/NS::SharedPtr<MTL::ResidencySet>{});
    return m_texture.get();
}

//======================================================================================================================
void Metal4Swapchain::resize(uint32_t width, uint32_t height) {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    LMX_ASSERT(width > 0 && height > 0, "Swapchain::resize: width and height must be positive");

    m_layer->setDrawableSize(CGSize{static_cast<CGFloat>(width), static_cast<CGFloat>(height)});
    LMX_LOG_INFO("swapchain resized to {}x{}", width, height);
}

//======================================================================================================================
void Metal4Swapchain::releaseCurrentDrawable() {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());
    m_texture.reset();
    m_drawable.reset();
}

} // namespace lmx::rhi::metal4
