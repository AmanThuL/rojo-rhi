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
/// severity and aborts. `cond` is evaluated once.
#define ROJORHI_VU(cond, id, ...)                                                                                                                              \
    do                                                                                                                                                         \
    {                                                                                                                                                          \
        if (!(cond))                                                                                                                                           \
        {                                                                                                                                                      \
            ::rojoRHI::base::emitMessage(::rojoRHI::MessageSeverity::Error, std::format("{}: {}", id, std::format(__VA_ARGS__)));                              \
            std::abort();                                                                                                                                      \
        }                                                                                                                                                      \
    } while (0)
#else
/// Release expands to nothing and evaluates neither the condition nor the message arguments.
#define ROJORHI_VU(cond, id, ...) ((void)0)
#endif
