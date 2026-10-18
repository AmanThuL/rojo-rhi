//----------------------------------------------------------------------------------------------------------------------
/// @file Conformance.h
/// @brief Declares the guard that conformance cases start with.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Support/Backend.h"

#include <catch2/catch_test_macros.hpp>

namespace rojoRHI::test {
    /// Skips the running case when the compiled backend executes no work on a GPU, because a conformance case observes what
    /// the GPU did.
    inline void requireGpu()
    {
        if (!runsOnGpu())
        {
            SKIP("this case observes GPU results, and the compiled backend runs no GPU");
        }
    }
} // namespace rojoRHI::test
