//----------------------------------------------------------------------------------------------------------------------
/// @file Swapchain.h
/// @brief Declares presentation swapchains bound to a native surface.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include "RHI/Format.h"
#include "RHI/Result.h"

#include <cstdint>

namespace lmx::rhi {

class Texture;

/// Describes a swapchain bound to a native presentation layer.
struct SwapchainDesc {
    void* nativeLayer = nullptr;        ///< Native CAMetalLayer supplied by the windowing layer.
    uint32_t width = 0, height = 0;     ///< Drawable extent in pixels.
    Format format = Format::BGRA8Unorm; ///< Presentation color format.
};
/// Lifetime: a Swapchain must not outlive the Device that created it, and must be destroyed
/// before the native surface it was built on. Destruction blocks until all GPU work referencing
/// the swapchain has completed — a backend may not release presentation resources out from under
/// in-flight command buffers — so no waitIdle() is required around it.
/// Owns presentation resources created by a device for one native surface.
class Swapchain {
public:
    /// Waits for outstanding use and destroys the presentation resources.
    virtual ~Swapchain() = default;
    /// Acquires the next presentation texture, valid through endFrame.
    virtual Result<Texture*> acquireNextTexture() = 0; // valid until endFrame/present
    /// Resizes the presentation surface.
    virtual void resize(uint32_t width, uint32_t height) = 0;
};

} // namespace lmx::rhi
