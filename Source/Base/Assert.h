//----------------------------------------------------------------------------------------------------------------------
/// @file Assert.h
/// @brief Defines process-fatal contract assertions used throughout the RHI.
//----------------------------------------------------------------------------------------------------------------------

#pragma once
#include "Base/Log.h"

#include <cstdlib>

/// Fatal contract check — enabled in ALL build configs (spec §6).
#define ROJORHI_ASSERT(cond, msg)                                                                      \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            ROJORHI_LOG_ERROR("ASSERT FAILED: {} ({}:{}) — {}", #cond, __FILE__, __LINE__, msg);       \
            std::abort();                                                                          \
        }                                                                                          \
    } while (0)
