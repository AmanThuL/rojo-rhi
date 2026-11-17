//----------------------------------------------------------------------------------------------------------------------
/// @file AbiExamples.h
/// @brief Declares the C++ side of the shader ABI chapter's example root, checked against the layout probe.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <cstddef>
#include <cstdint>

namespace rojoRHI::test {
    // The shader ABI chapter's `DrawRoot`: a flat 32-byte root block.
    struct DrawRoot
    {
        Addr vertices;
        Addr instances;
        // Heap indices of the albedo texture and the linear-clamp sampler.
        uint32_t albedo;
        uint32_t linearClamp;
        uint32_t drawId;
        uint32_t pad;
    };
    // Shared by the atomic-add entries and the reflection probe.
    struct alignas(16) AtomicRoot
    {
        Addr target; // The target base, or the address of the two-base table for the divergent entry.
        Addr output; // One returned old value per invocation.
        uint32_t count;
        uint32_t index; // Target uint index relative to the base.
        uint32_t value;
        uint32_t pad;
    };

    static_assert(alignof(AtomicRoot) == 16);
    static_assert(sizeof(AtomicRoot) == 32);
    static_assert(offsetof(AtomicRoot, target) == 0);
    static_assert(offsetof(AtomicRoot, output) == 8);
    static_assert(offsetof(AtomicRoot, count) == 16);
    static_assert(offsetof(AtomicRoot, index) == 20);
    static_assert(offsetof(AtomicRoot, value) == 24);
    static_assert(offsetof(AtomicRoot, pad) == 28);

    // Shared by the precise multiply-add entry and the reflection probe.
    struct alignas(16) PreciseRoot
    {
        Addr input;  // Four floats: the factors x and y, the addend z and an unused word.
        Addr output; // One float, x * y + z.
    };

    static_assert(alignof(PreciseRoot) == 16);
    static_assert(sizeof(PreciseRoot) == 16);
    static_assert(offsetof(PreciseRoot, input) == 0);
    static_assert(offsetof(PreciseRoot, output) == 8);
} // namespace rojoRHI::test
