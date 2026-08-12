//----------------------------------------------------------------------------------------------------------------------
/// @file Sampler.h
/// @brief Declares immutable samplers and their filtering, addressing, and comparison vocabulary.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include <cstdint>
#include <string_view>

namespace lmx::rhi {

/// Selects nearest or linear texture filtering.
enum class FilterMode {
    Nearest, ///< Selects the nearest sample.
    Linear   ///< Linearly interpolates neighboring samples.
};
/// Selects wrapping or clamping outside normalized texture coordinates.
enum class AddressMode {
    Wrap, ///< Repeats coordinates outside the normalized range.
    Clamp ///< Clamps coordinates to the texture edge.
};
/// Grows per demand. LessEqual is the shadow compare for a conventional depth buffer; GreaterEqual
/// is its reversed-Z counterpart, where the larger stored depth is the nearer surface.
/// Selects the depth comparison performed by a comparison sampler.
enum class CompareFunc {
    Never,       ///< Creates a non-comparison sampler.
    LessEqual,   ///< Passes when the sampled value is less than or equal to the reference.
    GreaterEqual ///< Passes when the sampled value is greater than or equal to the reference.
};
/// Describes immutable sampler filtering, addressing, anisotropy, and comparison behavior.
struct SamplerDesc {
    FilterMode filter = FilterMode::Linear; ///< Shared minification, magnification, and mip filter.
    AddressMode addressMode = AddressMode::Wrap; ///< Shared addressing mode for every axis.
    uint32_t maxAnisotropy = 1;               ///< Maximum anisotropy in the inclusive range 1..16.
    CompareFunc compare = CompareFunc::Never; ///< Comparison function; Never disables comparison.
    std::string_view label;                   ///< Diagnostic object label.
};
/// Immutable once created, and cheap enough that the backend keeps no cache: a sampler is a
/// handful of descriptor bits, so callers create the two or three the frame needs at startup
/// and hold them for the device's lifetime.
/// Represents an immutable GPU sampler.
class Sampler {
public:
    /// Destroys the sampler.
    virtual ~Sampler() = default;
};

} // namespace lmx::rhi
