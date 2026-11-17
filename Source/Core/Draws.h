//----------------------------------------------------------------------------------------------------------------------
/// @file Draws.h
/// @brief Declares the checks of the root, draw, dispatch and indirect command rules of chapters 05, 07 and 08.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"

#include <cstdint>

namespace rojoRHI::core {
    void setRoot(CommandListCore& list, Addr root);

    // A direct draw or dispatch with a zero count makes no use for the validation tracker.
    void draw(CommandListCore& list, uint32_t vertices, uint32_t instances);

    void drawIndexed(CommandListCore& list, Buffer indices, IndexType type, uint32_t count, uint32_t instances, uint32_t firstIndex);

    void drawMeshTasks(CommandListCore& list, uint32_t x, uint32_t y, uint32_t z);

    void dispatch(CommandListCore& list, uint32_t x, uint32_t y, uint32_t z);

    void drawIndirect(CommandListCore& list, Addr args, uint32_t maxCount, Addr count);

    void drawIndexedIndirect(CommandListCore& list, Buffer indices, Addr args, uint32_t maxCount, Addr count);

    void drawMeshTasksIndirect(CommandListCore& list, Addr args, uint32_t maxCount, Addr count);

    void dispatchIndirect(CommandListCore& list, Addr args);
} // namespace rojoRHI::core
