//----------------------------------------------------------------------------------------------------------------------
/// @file Barriers.h
/// @brief Declares the check of the barrier rules of chapter 09 that need no tracking.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"

#include <span>

namespace rojoRHI::core {
    // Checks only the rules that need no resource state; the rules that follow resource state are the tracker's.
    void barrier(
        CommandListCore& list, std::span<const GlobalBarrier> globals, std::span<const BufferBarrier> buffers, std::span<const TextureBarrier> textures
    );
} // namespace rojoRHI::core
