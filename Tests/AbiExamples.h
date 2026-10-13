//----------------------------------------------------------------------------------------------------------------------
/// @file AbiExamples.h
/// @brief Declares the C++ side of the shader ABI chapter's example root, checked against the layout probe.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Addr.h>

#include <cstdint>

namespace rojoRHI::test {
    /// The `DrawRoot` of the shader ABI chapter: a flat 32-byte root block.
    struct DrawRoot
    {
        /// Address of the vertex data.
        Addr vertices;
        /// Address of the instance data.
        Addr instances;
        /// Heap index of the albedo texture.
        uint32_t albedo;
        /// Heap index of the linear-clamp sampler.
        uint32_t linearClamp;
        /// Identifier of the draw.
        uint32_t drawId;
        /// Explicit padding up to 32 bytes.
        uint32_t pad;
    };
} // namespace rojoRHI::test
