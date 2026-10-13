//----------------------------------------------------------------------------------------------------------------------
/// @file Draws.h
/// @brief Declares the checks of the root, draw, dispatch and indirect command rules of chapters 05, 07 and 08.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Addr.h>
#include <rojoRHI/Handles.h>
#include <rojoRHI/IndexType.h>

#include "Core/CommandLists.h"

#include <cstdint>

namespace rojoRHI::core {
    /// Checks and records `root` as the list's root (VU-rojo-cmd-0063, VU-rojo-abi-0001, VU-rojo-abi-0002, and
    /// VU-rojo-buffer-0009 and VU-rojo-buffer-0020 through `resolveAddr`).
    void setRoot(CommandListCore& list, Addr root);

    /// Checks a direct draw of `instances` instances of `vertices` vertices (VU-rojo-cmd-0063, 0023, 0024). A draw with a zero
    /// count makes no use for the validation tracker; so it is for every direct draw and dispatch below.
    void draw(CommandListCore& list, uint32_t vertices, uint32_t instances);

    /// Checks an indexed draw of `instances` instances of `count` indices of `type` from `firstIndex` of `indices`
    /// (VU-rojo-cmd-0063, 0025, 0026, VU-rojo-core-0001 to 0003).
    void drawIndexed(CommandListCore& list, Buffer indices, IndexType type, uint32_t count, uint32_t instances, uint32_t firstIndex);

    /// Checks a mesh-task draw of an `x` by `y` by `z` grid (VU-rojo-cmd-0063, 0027, 0028).
    void drawMeshTasks(CommandListCore& list, uint32_t x, uint32_t y, uint32_t z);

    /// Checks a dispatch of an `x` by `y` by `z` grid (VU-rojo-cmd-0063, 0029 to 0031).
    void dispatch(CommandListCore& list, uint32_t x, uint32_t y, uint32_t z);

    /// Checks an indirect draw (VU-rojo-cmd-0063, VU-rojo-indirect-0001 to 0004, 0021, and the address rules of `resolveAddr`).
    void drawIndirect(CommandListCore& list, Addr args, uint32_t maxCount, Addr count);

    /// Checks an indexed indirect draw (VU-rojo-cmd-0063, VU-rojo-indirect-0006 to 0010, 0022, VU-rojo-core-0001 to 0003 for
    /// `indices`, and the address rules of `resolveAddr`).
    void drawIndexedIndirect(CommandListCore& list, Buffer indices, Addr args, uint32_t maxCount, Addr count);

    /// Checks a mesh-task indirect draw (VU-rojo-cmd-0063, VU-rojo-indirect-0012 to 0016, 0023, and the address rules of
    /// `resolveAddr`).
    void drawMeshTasksIndirect(CommandListCore& list, Addr args, uint32_t maxCount, Addr count);

    /// Checks an indirect dispatch (VU-rojo-cmd-0063, VU-rojo-indirect-0018, 0019, and the address rules of `resolveAddr`).
    void dispatchIndirect(CommandListCore& list, Addr args);
} // namespace rojoRHI::core
