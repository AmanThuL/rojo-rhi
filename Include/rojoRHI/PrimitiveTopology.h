//----------------------------------------------------------------------------------------------------------------------
/// @file PrimitiveTopology.h
/// @brief Declares the exact topology of a draw.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// The exact topology of the draws of a vertex pipeline; it belongs to the `PrimitiveClass` of the bound pipeline
    /// (VU-rojo-pipeline-0023). Primitive restart is not part of 0.1.0.
    enum class PrimitiveTopology : uint8_t
    {
        PointList,     ///< Points; class `Point`.
        LineList,      ///< Lines; class `Line`.
        LineStrip,     ///< Line strips; class `Line`.
        TriangleList,  ///< Triangles; class `Triangle`.
        TriangleStrip, ///< Triangle strips; class `Triangle`.
    };
} // namespace rojoRHI
