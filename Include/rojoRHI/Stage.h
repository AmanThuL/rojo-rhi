//----------------------------------------------------------------------------------------------------------------------
/// @file Stage.h
/// @brief Declares the set of pipeline stages a barrier names.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// A bit set of pipeline stages, modelled on D3D12 enhanced barriers and Vulkan synchronization2. As a source `None` means no
    /// earlier work, as a destination no later work. A combined bit lowers to the union of its members' mappings.
    enum class Stage : uint32_t
    {
        None = 0,                  ///< No stage.
        IndirectArgs = 1u << 0,    ///< Reading indirect argument records and draw counts.
        IndexInput = 1u << 1,      ///< Fetching indices for indexed draws.
        VertexShader = 1u << 2,    ///< Vertex shader execution.
        TaskShader = 1u << 3,      ///< Task shader execution.
        MeshShader = 1u << 4,      ///< Mesh shader execution.
        FragmentShader = 1u << 5,  ///< Fragment shader execution.
        ComputeShader = 1u << 6,   ///< Compute shader execution.
        ColorAttachment = 1u << 7, ///< Color attachment loads, blending and stores, including load-operation clears.
        DepthStencil = 1u << 8,    ///< Depth and stencil tests, writes and load-operation clears.
        Copy = 1u << 9,            ///< Every copy command, `fillBuffer` and `clearTexture`.
        Resolve = 1u << 10,        ///< The multisample resolve at the end of a render pass.
        /// Every stage a render pass and its draws use.
        AllGraphics = IndirectArgs | IndexInput | VertexShader | TaskShader | MeshShader | FragmentShader | ColorAttachment | DepthStencil | Resolve,
        All = (1u << 11) - 1, ///< Every stage.
    };

    /// Returns the union of two stage sets.
    constexpr Stage operator|(Stage a, Stage b)
    {
        return static_cast<Stage>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }

    /// Returns the stages both sets contain.
    constexpr Stage operator&(Stage a, Stage b)
    {
        return static_cast<Stage>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }

    /// Returns the complement of a stage set within the 32-bit mask.
    constexpr Stage operator~(Stage a)
    {
        return static_cast<Stage>(~static_cast<uint32_t>(a));
    }
} // namespace rojoRHI
