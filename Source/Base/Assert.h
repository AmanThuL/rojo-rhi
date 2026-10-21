//----------------------------------------------------------------------------------------------------------------------
/// @file Assert.h
/// @brief Defines the valid-usage check that runs in non-Release builds.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Base/Log.h"

#include <cstdlib>
#include <format>

#ifndef NDEBUG
/// Checks a valid-usage rule: when `cond` is false, sends "<id>: <formatted text>" to the message callback at error
/// severity, and to stderr as well, and aborts. `cond` is evaluated once.
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
/// Release expands to nothing and evaluates neither the condition nor the message arguments.
#define ROJORHI_VU(cond, id, ...) ((void)0)
#endif

#ifndef NDEBUG
/// Checks an invariant of the library's own code, never a rule a caller can break: when `cond` is false, reports `msg` at
/// error severity, on stderr as well, and aborts. Release expands to nothing and does not evaluate `cond`.
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
/// Release expands to nothing.
#define ROJORHI_INTERNAL_ASSERT(cond, msg) ((void)0)
#endif
