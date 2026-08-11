//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Swapchain.h
/// @brief Declares the Metal 4 swapchain backed by a Core Animation Metal layer.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include "Metal4Common.h"
#include "Metal4Resources.h"

#include <cstdint>
#include <memory>

namespace lmx::rhi::metal4 {

// Presentation surface: a thin adapter over the CAMetalLayer the windowing layer created.
//
// Drawable lifetime (single-threaded, one drawable in flight at a time by construction):
//   acquireNextTexture() takes a +1 reference on the layer's next drawable and keeps it here,
//   together with a transient Metal4Texture wrapping the drawable's texture. Metal4Device's
//   endFrame() reads them back through currentDrawable() to run the wait/commit/signal/present
//   sequence, then calls releaseCurrentDrawable(). Nothing else may hold either object past
//   that point -- the Texture* handed to the caller is documented as valid until endFrame.
//
// The drawable is retained explicitly rather than left autoreleased because acquire and
// present happen in different RHI calls, each with its own local autorelease pool.
//
// Destruction drains the queue first -- see the destructor. Callers therefore do NOT have to
// waitIdle before releasing a swapchain, which matters because the destruction order that makes
// the *device's* drain sufficient (device last) is exactly the order that puts it too late.
class Metal4Swapchain final : public Swapchain {
public:
    Metal4Swapchain(NS::SharedPtr<CA::MetalLayer> layer, Format format,
                    NS::SharedPtr<MTL4::CommandQueue> queue,
                    NS::SharedPtr<MTL::ResidencySet> layerResidency);
    ~Metal4Swapchain() override;

    Metal4Swapchain(const Metal4Swapchain&) = delete;
    Metal4Swapchain& operator=(const Metal4Swapchain&) = delete;

    Result<Texture*> acquireNextTexture() override;
    void resize(uint32_t width, uint32_t height) override;

    // endFrame's half of the handoff described above. currentDrawable() is null when no
    // texture is currently acquired.
    CA::MetalDrawable* currentDrawable() const { return m_drawable.get(); }
    void releaseCurrentDrawable();

private:
    NS::SharedPtr<CA::MetalLayer> m_layer;
    // The presentation format the device configured the layer with, carried so a drawable's
    // texture wrapper can report it without a Metal-to-RHI format lookup that exists nowhere else.
    Format m_format = Format::Unknown;
    // Held so the destructor can drain, and can detach the layer's residency set from the same
    // queue the constructor attached it to.
    NS::SharedPtr<MTL4::CommandQueue> m_queue;
    // The layer owns this set, but the queue retains it for as long as it is attached, so the
    // reference is made explicit here rather than cached as a raw pointer. Null when this build
    // of CAMetalLayer does not vend one.
    NS::SharedPtr<MTL::ResidencySet> m_layerResidency;

    NS::SharedPtr<CA::MetalDrawable> m_drawable;
    // Rebuilt on every acquire: each drawable brings its own texture, and Metal4Texture is
    // neither copyable nor movable (its ResidencyRegistration is pinned to the resource).
    std::unique_ptr<Metal4Texture> m_texture;
};

} // namespace lmx::rhi::metal4
