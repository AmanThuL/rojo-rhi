//----------------------------------------------------------------------------------------------------------------------
/// @file BlendState.h
/// @brief Declares the fixed-function blend state of one color target.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// A factor that scales a source or destination term of a blend equation.
    enum class BlendFactor : uint8_t
    {
        Zero,             ///< 0.
        One,              ///< 1.
        SrcColor,         ///< The fragment's color.
        OneMinusSrcColor, ///< 1 minus the fragment's color.
        SrcAlpha,         ///< The fragment's alpha.
        OneMinusSrcAlpha, ///< 1 minus the fragment's alpha.
        DstColor,         ///< The attachment's color.
        OneMinusDstColor, ///< 1 minus the attachment's color.
        DstAlpha,         ///< The attachment's alpha.
        OneMinusDstAlpha, ///< 1 minus the attachment's alpha.
        SrcAlphaSaturate, ///< The smaller of the fragment's alpha and 1 minus the attachment's alpha.
        Constant,         ///< The blend constants set on the command list.
        OneMinusConstant, ///< 1 minus the blend constants.
    };

    /// How the two scaled terms of a blend equation combine.
    enum class BlendOp : uint8_t
    {
        Add,             ///< Source plus destination.
        Subtract,        ///< Source minus destination.
        ReverseSubtract, ///< Destination minus source.
        Min,             ///< The smaller of the two; the factors are ignored.
        Max,             ///< The larger of the two; the factors are ignored.
    };

    /// Fixed-function blending for one color target, baked into the pipeline. With `enable` false the fragment output is
    /// written unchanged. Dual-source blending and logic operations are not part of 0.1.0.
    struct BlendState
    {
        /// True turns blending on; only for a format that reports `FormatCapabilities::blend` (VU-rojo-format-0003).
        bool enable = false;
        /// Factor of the fragment's color.
        BlendFactor srcColor = BlendFactor::One;
        /// Factor of the attachment's color.
        BlendFactor dstColor = BlendFactor::Zero;
        /// Combines the color terms.
        BlendOp colorOp = BlendOp::Add;
        /// Factor of the fragment's alpha.
        BlendFactor srcAlpha = BlendFactor::One;
        /// Factor of the attachment's alpha.
        BlendFactor dstAlpha = BlendFactor::Zero;
        /// Combines the alpha terms.
        BlendOp alphaOp = BlendOp::Add;
    };
} // namespace rojoRHI
