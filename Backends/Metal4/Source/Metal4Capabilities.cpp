//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Capabilities.cpp
/// @brief Implements the Metal 4 hardware floor check and the limits and floor name a device reports.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Capabilities.h"

#include <rojoRHI/RHI.h>

#include <Metal/Metal.hpp>

#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <utility>

namespace rojoRHI::metal4 {
    namespace {
        /// The floor's `maxSamplers`, chapter 15 part 2.
        constexpr uint64_t kFloorSamplers = 996;

        /// The floor's `maxBufferSize` in bytes: the floor's `maxAddrOffset`, 2^32 - 1, is `maxBufferLength` less one.
        constexpr uint64_t kFloorBufferLength = uint64_t{ 1 } << 32;

        /// The floor's `maxGroupSize` on each axis, chapter 15 part 2; the x axis is also the floor's `maxThreadsPerGroup`.
        constexpr uint64_t kFloorGroupWidth = 1'024;
        constexpr uint64_t kFloorGroupHeight = 1'024;
        constexpr uint64_t kFloorGroupDepth = 64;

        /// The floor's `maxGroupSharedMemory` in bytes, chapter 15 part 2.
        constexpr uint64_t kFloorGroupSharedMemory = 32'768;

        //==============================================================================================================
        Result<void> unsupported(std::string message)
        {
            return std::unexpected(Error{ ErrorCategory::Unsupported, std::move(message) });
        }
    } // namespace

    //==================================================================================================================
    FloorReport floorReport(MTL::Device* device)
    {
        FloorReport report;
        report.metal4Family = device->supportsFamily(MTL::GPUFamilyMetal4);
        report.apple7Family = device->supportsFamily(MTL::GPUFamilyApple7);
        report.readWriteTextureTier2 = device->readWriteTextureSupport() >= MTL::ReadWriteTextureTier2;
        report.samplers = device->maxArgumentBufferSamplerCount();
        report.maxBufferLength = device->maxBufferLength();
        report.maxThreadsPerThreadgroup = device->maxThreadsPerThreadgroup();
        report.maxThreadgroupMemoryLength = device->maxThreadgroupMemoryLength();
        report.bcTextureCompression = device->supportsBCTextureCompression();
        return report;
    }

    //==================================================================================================================
    Result<void> checkFloor(const FloorReport& report)
    {
        if (!report.metal4Family)
        {
            return unsupported("the GPU does not support the Metal 4 family (GPUFamilyMetal4)");
        }
        if (!report.apple7Family)
        {
            return unsupported("the GPU is below the Apple7 family (GPUFamilyApple7)");
        }
        if (!report.readWriteTextureTier2)
        {
            return unsupported("the GPU does not support read-write texture tier 2 (ReadWriteTextureTier2)");
        }
        if (report.samplers < kFloorSamplers)
        {
            return unsupported(std::format("the GPU's argument buffers hold {} samplers, fewer than the floor's {}", report.samplers, kFloorSamplers));
        }
        if (report.maxBufferLength < kFloorBufferLength)
        {
            return unsupported(std::format("the GPU's maxBufferLength is {} bytes, below the floor's {}", report.maxBufferLength, kFloorBufferLength));
        }
        const MTL::Size& threads = report.maxThreadsPerThreadgroup;
        if (threads.width < kFloorGroupWidth || threads.height < kFloorGroupHeight || threads.depth < kFloorGroupDepth)
        {
            return unsupported(
                std::format(
                    "the GPU's maxThreadsPerThreadgroup is {} x {} x {}, below the floor's {} x {} x {}",
                    threads.width,
                    threads.height,
                    threads.depth,
                    kFloorGroupWidth,
                    kFloorGroupHeight,
                    kFloorGroupDepth
                )
            );
        }
        if (report.maxThreadgroupMemoryLength < kFloorGroupSharedMemory)
        {
            return unsupported(
                std::format(
                    "the GPU's maxThreadgroupMemoryLength is {} bytes, below the floor's {}", report.maxThreadgroupMemoryLength, kFloorGroupSharedMemory
                )
            );
        }
        if (!report.bcTextureCompression)
        {
            return unsupported("the GPU does not support BC texture compression (supportsBCTextureCompression)");
        }
        return {};
    }

    //==================================================================================================================
    Result<void> checkFloor(MTL::Device* device)
    {
        return checkFloor(floorReport(device));
    }

    //==================================================================================================================
    Limits metalLimits(MTL::Device* device)
    {
        const MTL::Size threads = device->maxThreadsPerThreadgroup();
        Limits limits;
        limits.maxViews = kMaxViews;                                                         // Metal 4 column.
        limits.maxSamplers = static_cast<uint32_t>(device->maxArgumentBufferSamplerCount()); // Device.
        limits.maxRootBlockSize = 65'536;                                                    // Floor: the Metal 4 column bounds it by maxBufferLength alone.
        limits.rootAlignment = 256;                                                          // Metal 4 column: 4, raised to 256.
        limits.frameDataAlignment = 256;                                                     // Metal 4 column.
        limits.maxBufferSize = device->maxBufferLength();                                    // Device.
        limits.maxAddrOffset = limits.maxBufferSize - 1;                                     // Device: no byte lies further into a buffer.
        limits.maxTextureDimension1D = 16'384;                                               // Metal 4 column.
        limits.maxTextureDimension2D = 16'384;                                               // Metal 4 column.
        limits.maxTextureDimension3D = 2'048;                                                // Metal 4 column.
        limits.maxTextureDimensionCube = 16'384;                                             // Metal 4 column.
        limits.maxTextureLayers = 2'048;                                                     // Metal 4 column.
        limits.maxColorAttachments = 8;                                                      // Metal 4 column.
        limits.maxColorBytesPerSample = 64;                                                  // Floor: the Metal 4 column is unverified.
        limits.maxSamplerAnisotropy = 16;                                                    // Metal 4 column.
        limits.maxDrawIndirectCount = 65'536;                                                // Metal 4 column.
        limits.maxDispatchGroups = 65'535;                                                   // Floor: the Metal 4 column is unverified.
        limits.maxThreadsPerGroup = static_cast<uint32_t>(threads.width);                    // Device.
        limits.maxGroupSize[0] = static_cast<uint32_t>(threads.width);                       // Device.
        limits.maxGroupSize[1] = static_cast<uint32_t>(threads.height);                      // Device.
        limits.maxGroupSize[2] = static_cast<uint32_t>(threads.depth);                       // Device.
        limits.maxGroupSharedMemory = static_cast<uint32_t>(device->maxThreadgroupMemoryLength()); // Device.
        limits.maxMeshTaskGroups = 65'535;                                                         // Floor: the Metal 4 column is unverified.
        limits.maxMeshTaskGroupsTotal = 4'194'304;                                                 // Floor: the Metal 4 column is unverified.
        return limits;
    }

    //==================================================================================================================
    std::string floorName(MTL::Device* device)
    {
        constexpr std::array<std::pair<MTL::GPUFamily, const char*>, 4> families = { {
            { MTL::GPUFamilyApple10, "Apple10" },
            { MTL::GPUFamilyApple9, "Apple9" },
            { MTL::GPUFamilyApple8, "Apple8" },
            { MTL::GPUFamilyApple7, "Apple7" },
        } };
        for (const auto& [family, name] : families)
        {
            if (device->supportsFamily(family))
            {
                return name;
            }
        }
        // checkFloor has refused a device below Apple7.
        return "Apple7";
    }
} // namespace rojoRHI::metal4
