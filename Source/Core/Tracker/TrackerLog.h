//----------------------------------------------------------------------------------------------------------------------
/// @file TrackerLog.h
/// @brief Declares the log of tracker events a command list keeps and the recording hooks that append to it.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Tracker/Events.h"

#include <cstdint>
#include <span>
#include <unordered_set>
#include <vector>

namespace rojoRHI::core {
    struct CommandListCore;

    // Only a list of a device created with validation appends to it, and only in a Debug build; `Queue::submit` replays it.
    struct TrackerLog
    {
        std::vector<TrackerEvent> events;
        // The command of each event, at the same index. Every entry that logs counts its own command first, so the uses of one
        // command, and the records of one barrier call, share a value and no other event does.
        std::vector<uint64_t> commands;
        uint64_t command = 0;
        // The attachment uses of the open render pass, whose accesses its draws complete; empty outside a pass.
        std::vector<uint32_t> passUses;
        // Each referenced object once, holding a count in its record that submission or recycling removes.
        std::unordered_set<uint64_t> rootRefs;
        std::unordered_set<uint64_t> addrRefs;
        std::unordered_set<uint64_t> otherRefs;
        // The swapchain images in `otherRefs`, which replay checks are still the swapchain's (VU-rojo-present-0019). Releasing
        // the counts keeps it; `beginList` clears it with the rest of the log, so a recycled list names no earlier image.
        std::vector<Texture> images;
    };

    void logUseChecked(CommandListCore& list, const UseEvent& use);

    void logBarriersChecked(
        CommandListCore& list, std::span<const GlobalBarrier> globals, std::span<const BufferBarrier> buffers, std::span<const TextureBarrier> textures
    );

    void logPassChecked(CommandListCore& list, const RenderPassDesc& desc);

    void endPassLogChecked(CommandListCore& list);

    // A buffer use of no bytes is not logged. Inline, so a Release build compiles the call to nothing; a device without
    // validation logs nothing. The other hooks below are inline for the same reason.
    inline void logUse([[maybe_unused]] CommandListCore& list, [[maybe_unused]] const UseEvent& use)
    {
#ifndef NDEBUG
        logUseChecked(list, use);
#endif
    }

    // The globals, then the buffer barriers, then the texture barriers.
    inline void logBarriers(
        [[maybe_unused]] CommandListCore& list,
        [[maybe_unused]] std::span<const GlobalBarrier> globals,
        [[maybe_unused]] std::span<const BufferBarrier> buffers,
        [[maybe_unused]] std::span<const TextureBarrier> textures
    )
    {
#ifndef NDEBUG
        logBarriersChecked(list, globals, buffers, textures);
#endif
    }

    // Runs after `beginPass` sets the pass state. Each attachment use holds its load and store accesses; `logDraw` adds the
    // draws'.
    inline void logPass([[maybe_unused]] CommandListCore& list, [[maybe_unused]] const RenderPassDesc& desc)
    {
#ifndef NDEBUG
        logPassChecked(list, desc);
#endif
    }

    void logDrawChecked(CommandListCore& list);

    // Adds a blended color target's read and the depth and stencil writes. It reads the pipeline `bindPipeline` cached on the
    // list, so it takes no lock.
    inline void logDraw([[maybe_unused]] CommandListCore& list)
    {
#ifndef NDEBUG
        logDrawChecked(list);
#endif
    }

    inline void endPassLog([[maybe_unused]] CommandListCore& list)
    {
#ifndef NDEBUG
        endPassLogChecked(list);
#endif
    }
} // namespace rojoRHI::core
