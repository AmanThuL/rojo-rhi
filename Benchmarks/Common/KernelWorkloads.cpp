//----------------------------------------------------------------------------------------------------------------------
/// @file KernelWorkloads.cpp
/// @brief Registers the GPU kernels G1 and G2 and defines the CPU oracles and data their arms and checks share.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/KernelArms.h"
#include "Common/KernelPatterns.h"
#include "Common/Workloads.h"

#include <array>
#include <format>
#include <string>
#include <string_view>

namespace rojoRHI::bench {
    namespace {
        constexpr std::array<std::string_view, 2> kPatterns{ "uniform", "divergent" };

        //==============================================================================================================
        Config kernelConfig(std::string_view workload, std::string_view pattern)
        {
            return Config{ .id = std::format("{}-{}", workload, pattern), .workload = std::string(workload), .pattern = std::string(pattern) };
        }
    } // namespace

    //==================================================================================================================
    DataEntry expectedLoads(uint32_t thread, bool divergent)
    {
        const uint32_t key = kernelKey(thread, divergent);
        DataEntry sum{};
        for (uint32_t j = 0; j < kKernelLoads; ++j)
        {
            const DataEntry entry = dataEntry(kernelIndex(key, j, kDataEntries));
            for (uint32_t word = 0; word < 4; ++word)
            {
                sum[word] += entry[word];
            }
        }
        return sum;
    }

    //==================================================================================================================
    float expectedSamples(uint32_t thread, bool divergent)
    {
        const uint32_t key = kernelKey(thread, divergent);
        uint32_t sum = 0;
        for (uint32_t j = 0; j < kKernelLoads; ++j)
        {
            sum += texelValue(kernelIndex(key, j, kTextureCount), sampleX(j), sampleY(j));
        }
        return static_cast<float>(sum);
    }

    //==================================================================================================================
    std::vector<DataEntry> dataTable()
    {
        std::vector<DataEntry> table;
        table.reserve(kDataEntries);
        for (uint32_t i = 0; i < kDataEntries; ++i)
        {
            table.push_back(dataEntry(i));
        }
        return table;
    }

    //==================================================================================================================
    std::vector<float> textureTexels(uint32_t t)
    {
        std::vector<float> texels;
        texels.reserve(kTextureSize * kTextureSize);
        for (uint32_t y = 0; y < kTextureSize; ++y)
        {
            for (uint32_t x = 0; x < kTextureSize; ++x)
            {
                texels.push_back(static_cast<float>(texelValue(t, x, y)));
            }
        }
        return texels;
    }

    //==================================================================================================================
    void registerKernelWorkloads(std::vector<WorkloadEntry>& entries)
    {
        for (const std::string_view pattern : kPatterns)
        {
            entries.push_back(makeEntry(kernelConfig("G1", pattern), runRojoG1, runNativeG1));
        }
        for (const std::string_view pattern : kPatterns)
        {
            entries.push_back(makeEntry(kernelConfig("G2", pattern), runRojoG2, runNativeG2, runFixedG2));
        }
    }
} // namespace rojoRHI::bench
