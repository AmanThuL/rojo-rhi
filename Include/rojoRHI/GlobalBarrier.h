//----------------------------------------------------------------------------------------------------------------------
/// @file GlobalBarrier.h
/// @brief Declares the barrier that orders all memory with no resource named.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Access.h>
#include <rojoRHI/Stage.h>

namespace rojoRHI {
    /// A memory dependency over all memory, with no resource named and no layout changed. `srcStage` and `dstStage` are not both
    /// `Stage::None` (VU-rojo-sync-0008); the sides obey VU-rojo-sync-0001 to 0003.
    struct GlobalBarrier
    {
        /// Stages of earlier work the barrier waits for.
        Stage srcStage = Stage::None;
        /// Accesses of earlier work the barrier makes visible.
        Access srcAccess = Access::None;
        /// Stages of later work that wait.
        Stage dstStage = Stage::None;
        /// Accesses of later work that see the source writes.
        Access dstAccess = Access::None;
    };
} // namespace rojoRHI
