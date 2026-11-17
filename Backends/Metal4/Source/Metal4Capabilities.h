//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Capabilities.h
/// @brief Declares the Metal 4 hardware floor check and the limits and floor name a device reports.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <Metal/Metal.hpp>

#include <cstdint>
#include <string>

namespace rojoRHI::metal4 {
    // Chapter 15's Metal 4 column: the textures argument buffers reach per stage.
    inline constexpr uint32_t kMaxViews = 1'000'000;

    struct FloorReport
    {
        bool metal4Family = false;
        bool apple7Family = false;
        bool readWriteTextureTier2 = false;
        uint64_t samplers = 0;                   // The `maxSamplers` limit.
        uint64_t maxBufferLength = 0;            // The `maxBufferSize` limit; `maxAddrOffset` is one less.
        MTL::Size maxThreadsPerThreadgroup{};    // The `maxThreadsPerGroup` and `maxGroupSize` limits.
        uint64_t maxThreadgroupMemoryLength = 0; // The `maxGroupSharedMemory` limit.
        bool bcTextureCompression = false;       // The floor's BC1 formats need it.
    };

    FloorReport floorReport(MTL::Device* device);

    // Names the first requirement missing from chapter 02's floor or a chapter 15 floor value that Metal reports.
    Result<void> checkFloor(const FloorReport& report);

    Result<void> checkFloor(MTL::Device* device);

    Limits metalLimits(MTL::Device* device);

    std::string floorName(MTL::Device* device);
} // namespace rojoRHI::metal4
