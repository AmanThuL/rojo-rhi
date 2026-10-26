//----------------------------------------------------------------------------------------------------------------------
/// @file AbiExamples.h
/// @brief Declares the C++ side of the shader ABI chapter's example root, checked against the layout probe.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <cstddef>
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
    /// The flat root shared by the atomic-add entries and the reflection probe.
    struct alignas(16) AtomicRoot
    {
        Addr target;    ///< The target base, or the address of the two-base table for the divergent entry.
        Addr output;    ///< One returned old value per invocation.
        uint32_t count; ///< Participating invocations.
        uint32_t index; ///< Target uint index relative to the base.
        uint32_t value; ///< Amount added by each invocation.
        uint32_t pad;   ///< Explicit padding to 32 bytes.
    };

    static_assert(alignof(AtomicRoot) == 16);
    static_assert(sizeof(AtomicRoot) == 32);
    static_assert(offsetof(AtomicRoot, target) == 0);
    static_assert(offsetof(AtomicRoot, output) == 8);
    static_assert(offsetof(AtomicRoot, count) == 16);
    static_assert(offsetof(AtomicRoot, index) == 20);
    static_assert(offsetof(AtomicRoot, value) == 24);
    static_assert(offsetof(AtomicRoot, pad) == 28);

    /// The dynamic precision probe's flat 16-byte root block.
    struct alignas(16) PreciseRoot
    {
        Addr input;
        Addr output;
    };

    static_assert(alignof(PreciseRoot) == 16);
    static_assert(sizeof(PreciseRoot) == 16);
    static_assert(offsetof(PreciseRoot, input) == 0);
    static_assert(offsetof(PreciseRoot, output) == 8);
} // namespace rojoRHI::test
