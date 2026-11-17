//----------------------------------------------------------------------------------------------------------------------
/// @file Conformance.h
/// @brief Declares the guards that conformance cases and the cases that submit an upscale start with.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Support/Backend.h"

#include <catch2/catch_test_macros.hpp>

#include <string_view>

namespace rojoRHI::test {
    inline void requireGpu()
    {
        if (!runsOnGpu())
        {
            SKIP("this case observes GPU results, and the compiled backend runs no GPU");
        }
    }

    inline void requireUpscaleCompletion()
    {
        const std::string_view reason = upscalesNeverComplete();
        if (!reason.empty())
        {
            SKIP("this case waits for an upscale, and " << reason);
        }
    }
} // namespace rojoRHI::test
