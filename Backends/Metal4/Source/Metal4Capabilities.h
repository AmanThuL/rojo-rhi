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
    /// The `maxViews` every Metal 4 device reports: the textures argument buffers reach per stage, chapter 15's Metal 4 column.
    inline constexpr uint32_t kMaxViews = 1'000'000;

    /// What a device reports for each item of the hardware floor it can be asked about.
    struct FloorReport
    {
        bool metal4Family = false;               ///< `supportsFamily(MTL::GPUFamilyMetal4)`.
        bool apple7Family = false;               ///< `supportsFamily(MTL::GPUFamilyApple7)`.
        bool readWriteTextureTier2 = false;      ///< `readWriteTextureSupport` is at least `MTL::ReadWriteTextureTier2`.
        uint64_t samplers = 0;                   ///< `maxArgumentBufferSamplerCount`, the `maxSamplers` limit.
        uint64_t maxBufferLength = 0;            ///< `maxBufferLength`, the `maxBufferSize` limit; `maxAddrOffset` is one less.
        MTL::Size maxThreadsPerThreadgroup{};    ///< `maxThreadsPerThreadgroup`, the `maxThreadsPerGroup` and `maxGroupSize` limits.
        uint64_t maxThreadgroupMemoryLength = 0; ///< `maxThreadgroupMemoryLength`, the `maxGroupSharedMemory` limit.
        bool bcTextureCompression = false;       ///< `supportsBCTextureCompression`, which the floor's BC1 formats need.
    };

    /// Returns what `device` reports for the hardware floor.
    FloorReport floorReport(MTL::Device* device);

    /// Returns `ErrorCategory::Unsupported`, naming the first requirement missing, when `report` is below the hardware floor of
    /// chapter 02 or below a floor value of chapter 15 that Metal reports: the Metal 4 family, the Apple7 family, read-write
    /// texture tier 2, 996 argument-buffer samplers, a `maxBufferLength` of 2^32 bytes (a `maxAddrOffset` of 2^32 - 1), 1,024
    /// x 1,024 x 64 threads per threadgroup, 32,768 bytes of threadgroup memory and BC texture compression.
    Result<void> checkFloor(const FloorReport& report);

    /// Checks the floor `device` reports, as the overload above does.
    Result<void> checkFloor(MTL::Device* device);

    /// Returns the limits of `device`: its own values where it states them, chapter 15's Metal 4 column elsewhere, and the
    /// floor where that column is unverified.
    Limits metalLimits(MTL::Device* device);

    /// Returns the name of the highest Apple family from Apple10 down to Apple7 that `device` supports.
    std::string floorName(MTL::Device* device);
} // namespace rojoRHI::metal4
