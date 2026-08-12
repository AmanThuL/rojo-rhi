//----------------------------------------------------------------------------------------------------------------------
/// @file RenderPass.h
/// @brief Declares the render-pass attachment, clear, and store descriptor.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include <string_view>

namespace lmx::rhi {

class Texture;

/// Describes render-pass attachments, clear operations, and its diagnostic label.
struct RenderPassDesc {
    Texture* colorTarget = nullptr; ///< Color attachment, or null for a depth-only pass.
    float clearColor[4] = {0.f, 0.f, 0.f, 1.f}; ///< Scene-linear RGBA clear value.
    bool clear = true;                          ///< Clears the color attachment when true.
    Texture* depthTarget = nullptr;             ///< Depth attachment, or null when unused.
    float clearDepth = 1.0f;                    ///< Depth clear value.
    bool storeDepth = false;                    ///< Preserves depth after the pass when true.
    /// Names the pass in GPU captures and validation diagnostics. Backends provide a stable
    /// fallback for an empty label, but production passes should use a subsystem-qualified name.
    std::string_view label;
};

} // namespace lmx::rhi
