//----------------------------------------------------------------------------------------------------------------------
/// @file RasterState.h
/// @brief Declares the rasterizer state baked into a graphics pipeline.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// Which faces are discarded.
    enum class CullMode : uint8_t
    {
        None,  ///< No face is discarded.
        Front, ///< Front faces are discarded.
        Back,  ///< Back faces are discarded.
    };

    /// Which winding order is a front face.
    enum class FrontFace : uint8_t
    {
        CounterClockwise, ///< Counter-clockwise triangles are front faces.
        Clockwise,        ///< Clockwise triangles are front faces.
    };

    /// How triangles are filled.
    enum class FillMode : uint8_t
    {
        Solid,     ///< Filled.
        Wireframe, ///< Edges only.
    };

    /// Rasterizer state baked into a graphics pipeline. With `depthBias` false, depth-bias values set on the command list are
    /// ignored; with it true they apply.
    struct RasterState
    {
        /// Faces discarded.
        CullMode cull = CullMode::None;
        /// Winding order of a front face.
        FrontFace frontFace = FrontFace::CounterClockwise;
        /// Triangle fill.
        FillMode fill = FillMode::Solid;
        /// False clamps fragment depth instead of clipping.
        bool depthClip = true;
        /// Enables the values of `CommandList::setDepthBias`.
        bool depthBias = false;
        /// Conservative rasterization; true only when `Capabilities::conservativeRaster` is (VU-rojo-pipeline-0006).
        bool conservative = false;
    };
} // namespace rojoRHI
