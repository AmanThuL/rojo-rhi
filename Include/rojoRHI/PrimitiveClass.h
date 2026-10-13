//----------------------------------------------------------------------------------------------------------------------
/// @file PrimitiveClass.h
/// @brief Declares the topology class baked into a graphics pipeline.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// The topology class baked into a graphics pipeline with a vertex stage; the exact topology within the class is dynamic
    /// (`CommandList::setPrimitiveTopology`). A mesh pipeline ignores the class: its mesh entry point declares its output
    /// topology.
    enum class PrimitiveClass : uint8_t
    {
        Point,    ///< Points.
        Line,     ///< Lines and line strips.
        Triangle, ///< Triangles and triangle strips.
    };
} // namespace rojoRHI
