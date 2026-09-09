//----------------------------------------------------------------------------------------------------------------------
/// @file RenderPass.h
/// @brief Declares the render-pass attachment, clear, and store descriptor.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include <cstdint>
#include <string_view>

namespace lmx::rhi {

class Texture;

/// Colour attachments a pass may declare beyond `RenderPassDesc::colorTarget`.
///
/// A fixed maximum keeps every descriptor a plain aggregate with no allocation behind it. Grows
/// per real feature demand, exactly like the rest of this header (ADR 0004).
inline constexpr uint32_t kMaxExtraColorTargets = 3;

/// Describes one colour attachment past the primary one, and how the pass loads it.
///
/// Each extra shares the primary attachment's extent and is written by the fragment output one
/// past the primary's: `extraColor[0]` is attachment 1, which a shader writes as `SV_Target1`.
struct ExtraColorTarget {
    Texture* target = nullptr;                  ///< Attachment texture; never null when counted.
    float clearColor[4] = {0.f, 0.f, 0.f, 1.f}; ///< Clear value in the attachment's own channels.
    bool clear = true;                          ///< Clears the attachment when true, else loads.
};

/// Describes render-pass attachments, clear operations, and its diagnostic label.
struct RenderPassDesc {
    Texture* colorTarget = nullptr; ///< Color attachment, or null for a depth-only pass.
    float clearColor[4] = {0.f, 0.f, 0.f, 1.f}; ///< Scene-linear RGBA clear value.
    bool clear = true;                          ///< Clears the color attachment when true.
    Texture* depthTarget = nullptr;             ///< Depth attachment, or null when unused.
    float clearDepth = 1.0f;                    ///< Depth clear value.
    bool storeDepth = false;                    ///< Preserves depth after the pass when true.
    /// Colour attachments 1..extraColorCount, in attachment order. Entries past the count are
    /// ignored; every counted one requires `colorTarget`, since attachment zero is where the
    /// primary lives.
    ExtraColorTarget extraColor[kMaxExtraColorTargets];
    uint32_t extraColorCount = 0; ///< Entries of `extraColor` the pass declares (<= the maximum).
    /// Origin-anchored viewport and scissor; 0/0 means the whole attachment. Both zero or both
    /// non-zero, neither larger than the attachments' extent. The load action still clears the
    /// whole attachment, so texels outside the area hold the clear value.
    uint32_t renderAreaWidth = 0;
    uint32_t renderAreaHeight = 0; ///< Height of the origin-anchored render area; see the width.
    /// Names the pass in GPU captures and validation diagnostics. Backends provide a stable
    /// fallback for an empty label, but production passes should use a subsystem-qualified name.
    std::string_view label;
};

} // namespace lmx::rhi
