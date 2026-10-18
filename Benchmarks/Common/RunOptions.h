//----------------------------------------------------------------------------------------------------------------------
/// @file RunOptions.h
/// @brief Declares how one arm is run: the mode, its frame counts, validation, dumps and the Null call trace.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace rojoRHI::bench {
    enum class Mode : uint8_t
    {
        Correctness,
        Timing,
    };

    struct RunOptions
    {
        Mode mode = Mode::Correctness;
        uint32_t warmupFrames = 2;
        uint32_t measuredFrames = 3;
        bool validation = false;             ///< DeviceDesc::validation of the RojoRHI arm; Debug builds only.
        std::filesystem::path dumpDirectory; ///< Empty: no dump.
        bool trace = false;                  ///< Null builds: keep the RojoRHI calls of the first measured frame.
    };

    // "correctness" or "timing".
    inline std::string_view modeName(Mode mode)
    {
        return mode == Mode::Timing ? "timing" : "correctness";
    }

    // Timing: 50 warm-up and 200 measured frames; correctness: 2 and 3, the last frame's outputs dumped.
    inline RunOptions runOptions(Mode mode)
    {
        RunOptions options;
        options.mode = mode;
        options.warmupFrames = mode == Mode::Timing ? 50 : 2;
        options.measuredFrames = mode == Mode::Timing ? 200 : 3;
        return options;
    }
} // namespace rojoRHI::bench
