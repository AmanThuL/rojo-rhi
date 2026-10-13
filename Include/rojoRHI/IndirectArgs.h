//----------------------------------------------------------------------------------------------------------------------
/// @file IndirectArgs.h
/// @brief Declares the argument records the indirect commands read from GPU memory.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// One non-indexed draw read by `CommandList::drawIndirect`; 20 bytes, 4-byte aligned.
    struct DrawIndirectArgs
    {
        /// Caller-defined value that `drawId()` returns in every invocation of this draw.
        uint32_t drawId;
        /// Vertices per instance; zero makes the record a no-op.
        uint32_t vertexCount;
        /// Instances to draw.
        uint32_t instanceCount;
        /// First vertex drawn.
        uint32_t firstVertex;
        /// First instance index.
        uint32_t firstInstance;
    };

    /// One indexed draw read by `CommandList::drawIndexedIndirect`; 24 bytes, 4-byte aligned.
    struct DrawIndexedIndirectArgs
    {
        /// Caller-defined value that `drawId()` returns in every invocation of this draw.
        uint32_t drawId;
        /// Indices per instance.
        uint32_t indexCount;
        /// Instances to draw.
        uint32_t instanceCount;
        /// First element read from the index buffer.
        uint32_t firstIndex;
        /// Signed value added to each fetched index.
        int32_t baseVertex;
        /// First instance index.
        uint32_t firstInstance;
    };

    /// One mesh-task draw read by `CommandList::drawMeshTasksIndirect`; 16 bytes, 4-byte aligned.
    struct DrawMeshTasksIndirectArgs
    {
        /// Caller-defined value that `drawId()` returns in every invocation of this draw.
        uint32_t drawId;
        /// Threadgroup count in x; any zero count makes the record a no-op.
        uint32_t x;
        /// Threadgroup count in y.
        uint32_t y;
        /// Threadgroup count in z.
        uint32_t z;
    };

    /// One compute dispatch read by `CommandList::dispatchIndirect`; 12 bytes, 4-byte aligned.
    struct DispatchIndirectArgs
    {
        /// Threadgroup count in x; any zero count makes the dispatch a no-op.
        uint32_t x;
        /// Threadgroup count in y.
        uint32_t y;
        /// Threadgroup count in z.
        uint32_t z;
    };
} // namespace rojoRHI
