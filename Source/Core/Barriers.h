//----------------------------------------------------------------------------------------------------------------------
/// @file Barriers.h
/// @brief Declares the check of the barrier rules of chapter 09 that need no tracking.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/BufferBarrier.h>
#include <rojoRHI/GlobalBarrier.h>
#include <rojoRHI/TextureBarrier.h>

#include "Core/CommandLists.h"

#include <span>

namespace rojoRHI::core {
    /// Checks one batch of barriers (VU-rojo-cmd-0063, VU-rojo-sync-0001 to 0003, 0005, 0007, 0008, 0010, 0011, 0014 to 0016, 0019,
    /// 0020, VU-rojo-texture-0016, 0017, 0041, VU-rojo-core-0001 to 0003). The rules that follow resource state are the
    /// validation tracker's.
    void barrier(
        CommandListCore& list, std::span<const GlobalBarrier> globals, std::span<const BufferBarrier> buffers, std::span<const TextureBarrier> textures
    );
} // namespace rojoRHI::core
