//----------------------------------------------------------------------------------------------------------------------
/// @file Assert.h
/// @brief Defines the valid-usage check that runs in non-Release builds.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Base/Log.h"

#include <cstdlib>
#include <format>

#ifndef NDEBUG
// Reports "<id>: <text>" to the message callback and to stderr, then aborts. `cond` is evaluated once.
#define ROJORHI_VU(cond, id, ...)                                                                                                                              \
    do                                                                                                                                                         \
    {                                                                                                                                                          \
        if (!(cond))                                                                                                                                           \
        {                                                                                                                                                      \
            ::rojoRHI::base::emitFatal(std::format("{}: {}", id, std::format(__VA_ARGS__)));                                                                   \
            std::abort();                                                                                                                                      \
        }                                                                                                                                                      \
    } while (0)
#else
// Release evaluates neither the condition nor the message arguments.
#define ROJORHI_VU(cond, id, ...) ((void)0)
#endif

#ifndef NDEBUG
// For the library's own invariants, never a rule a caller can break.
#define ROJORHI_INTERNAL_ASSERT(cond, msg)                                                                                                                     \
    do                                                                                                                                                         \
    {                                                                                                                                                          \
        if (!(cond))                                                                                                                                           \
        {                                                                                                                                                      \
            ::rojoRHI::base::emitFatal(std::format("internal error: {} ({}:{})", msg, __FILE__, __LINE__));                                                    \
            std::abort();                                                                                                                                      \
        }                                                                                                                                                      \
    } while (0)
#else
#define ROJORHI_INTERNAL_ASSERT(cond, msg) ((void)0)
#endif
