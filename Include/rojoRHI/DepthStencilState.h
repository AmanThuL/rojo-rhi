//----------------------------------------------------------------------------------------------------------------------
/// @file DepthStencilState.h
/// @brief Declares the depth and stencil state baked into a graphics pipeline.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/CompareOp.h>

#include <cstdint>

namespace rojoRHI {
    /// What a stencil test does to the stored stencil value.
    enum class StencilOp : uint8_t
    {
        Keep,           ///< Leaves the value.
        Zero,           ///< Sets the value to 0.
        Replace,        ///< Sets the value to the reference of `CommandList::setStencilReference`.
        IncrementClamp, ///< Adds 1, saturating at the maximum.
        DecrementClamp, ///< Subtracts 1, saturating at 0.
        Invert,         ///< Inverts the bits.
        IncrementWrap,  ///< Adds 1, wrapping to 0.
        DecrementWrap,  ///< Subtracts 1, wrapping to the maximum.
    };

    /// The stencil operations and comparison of one face orientation.
    struct StencilFaceState
    {
        /// Applied when the stencil test fails.
        StencilOp fail = StencilOp::Keep;
        /// Applied when the stencil test passes and the depth test fails.
        StencilOp depthFail = StencilOp::Keep;
        /// Applied when both tests pass.
        StencilOp pass = StencilOp::Keep;
        /// The stencil comparison.
        CompareOp compare = CompareOp::Always;
    };

    /// Depth test, depth write, compare and stencil operations, baked into the pipeline. `depthWrite` is true only when
    /// `depthTest` is (VU-rojo-pipeline-0007). Depth bounds testing is not part of 0.1.0.
    struct DepthStencilState
    {
        /// Enables the depth test.
        bool depthTest = false;
        /// Writes passing depth values.
        bool depthWrite = false;
        /// The depth comparison; reverse Z, matching clear depth 0.
        CompareOp depthCompare = CompareOp::GreaterOrEqual;
        /// Enables the stencil test.
        bool stencilTest = false;
        /// Bits of the stencil value the comparison reads.
        uint8_t stencilReadMask = 0xFF;
        /// Bits of the stencil value a write changes.
        uint8_t stencilWriteMask = 0xFF;
        /// Operations for front faces.
        StencilFaceState front;
        /// Operations for back faces.
        StencilFaceState back;
    };
} // namespace rojoRHI
