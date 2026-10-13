//----------------------------------------------------------------------------------------------------------------------
/// @file TrackerLog.h
/// @brief Declares the log of tracker events a command list keeps and the recording hooks that append to it.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/BufferBarrier.h>
#include <rojoRHI/GlobalBarrier.h>
#include <rojoRHI/RenderPassDesc.h>
#include <rojoRHI/TextureBarrier.h>

#include "Core/Tracker/Events.h"

#include <cstdint>
#include <span>
#include <vector>

namespace rojoRHI::core {
    struct CommandListCore;

    /// The events a list recorded for the validation tracker, in recording order. Only a list of a device created with
    /// validation appends to it, and only in a Debug build; `Queue::submit` replays it.
    struct TrackerLog
    {
        std::vector<TrackerEvent> events; ///< Every use and barrier record, in recording order.
        /// The positions in `events` of the attachment uses of the open render pass, whose accesses the draws of the pass
        /// complete; empty outside a pass.
        std::vector<uint32_t> passUses;
    };

    /// Does the work of `logUse` in a Debug build.
    void logUseChecked(CommandListCore& list, const UseEvent& use);

    /// Does the work of `logBarriers` in a Debug build.
    void logBarriersChecked(
        CommandListCore& list, std::span<const GlobalBarrier> globals, std::span<const BufferBarrier> buffers, std::span<const TextureBarrier> textures
    );

    /// Does the work of `logPass` in a Debug build.
    void logPassChecked(CommandListCore& list, const RenderPassDesc& desc);

    /// Does the work of `endPassLog` in a Debug build.
    void endPassLogChecked(CommandListCore& list);

    /// Appends `use` to the log of `list`, marked with whether the list is a Copy list. A buffer use of no bytes is no use and
    /// is not logged. Inline, so a Release build compiles the call to nothing; a device without validation logs nothing.
    inline void logUse([[maybe_unused]] CommandListCore& list, [[maybe_unused]] const UseEvent& use)
    {
#ifndef NDEBUG
        logUseChecked(list, use);
#endif
    }

    /// Appends each record of one barrier batch to the log of `list`: the globals, then the buffer barriers, then the texture
    /// barriers. Inline, as `logUse` is.
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

    /// Appends one use per attachment of the pass `desc` begins on `list`, whose state `beginPass` has just set, and notes
    /// their positions in `TrackerLog::passUses`. Each use holds the accesses the load and store operations make; the draws of
    /// the pass may add accesses to it. Inline, as `logUse` is.
    inline void logPass([[maybe_unused]] CommandListCore& list, [[maybe_unused]] const RenderPassDesc& desc)
    {
#ifndef NDEBUG
        logPassChecked(list, desc);
#endif
    }

    /// Does the work of `logDraw` in a Debug build.
    void logDrawChecked(CommandListCore& list);

    /// Completes the attachment uses of the open pass with what a draw of the bound pipeline adds: the read of each color target
    /// it blends, the depth write and the stencil write. It reads the pipeline `bindPipeline` cached on the list, so it takes no
    /// lock. Inline, as `logUse` is.
    inline void logDraw([[maybe_unused]] CommandListCore& list)
    {
#ifndef NDEBUG
        logDrawChecked(list);
#endif
    }

    /// Forgets the attachment uses of the pass that `list` ends: later draws belong to no pass. Inline, as `logUse` is.
    inline void endPassLog([[maybe_unused]] CommandListCore& list)
    {
#ifndef NDEBUG
        endPassLogChecked(list);
#endif
    }
} // namespace rojoRHI::core
