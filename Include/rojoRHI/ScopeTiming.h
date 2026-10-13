//----------------------------------------------------------------------------------------------------------------------
/// @file ScopeTiming.h
/// @brief Declares the GPU duration of one debug scope.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/QueueType.h>

#include <cstdint>
#include <string_view>

namespace rojoRHI {
    /// The GPU duration of one debug scope, as `Device::scopeTimings` reports it; every render pass and upscale forms a scope
    /// of its own. Durations are wall time: nested scopes overlap their parent, and scopes on different queues overlap each
    /// other.
    struct ScopeTiming
    {
        /// The scope's label, or the render pass's or upscale's, with the default label substituted for an empty one.
        std::string_view label;
        /// Number of the frame the scope was recorded in, counting `Device::beginFrame` calls from one.
        uint64_t frame = 0;
        /// Queue type of the command list that recorded the scope.
        QueueType queue = QueueType::Graphics;
        /// Nesting depth; zero for an outermost scope.
        uint32_t depth = 0;
        /// GPU wall time between the scope's two timestamps, in milliseconds; zero on the Null backend.
        double gpuMilliseconds = 0.0;
    };
} // namespace rojoRHI
