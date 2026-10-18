//----------------------------------------------------------------------------------------------------------------------
/// @file CounterFields.h
/// @brief Declares the twenty fields of rojoRHI::Counters by name, and the field-by-field delta of two snapshots.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace rojoRHI::bench {
    struct CounterField
    {
        std::string_view name;
        uint64_t Counters::* member;
    };

    // In declaration order, which is the order the run JSON writes them.
    inline constexpr std::array<CounterField, 20> kCounterFields{ {
        { "nativeCreateCalls", &Counters::nativeCreateCalls },     { "nativeRecordCalls", &Counters::nativeRecordCalls },
        { "nativeSubmitCalls", &Counters::nativeSubmitCalls },     { "nativeHostCalls", &Counters::nativeHostCalls },
        { "cpuAllocations", &Counters::cpuAllocations },           { "gpuAllocations", &Counters::gpuAllocations },
        { "frameDataUploads", &Counters::frameDataUploads },       { "frameArenaBytes", &Counters::frameArenaBytes },
        { "frameArenaGrowths", &Counters::frameArenaGrowths },     { "descriptorWrites", &Counters::descriptorWrites },
        { "globalBarriers", &Counters::globalBarriers },           { "bufferBarriers", &Counters::bufferBarriers },
        { "textureBarriers", &Counters::textureBarriers },         { "nativeBarriers", &Counters::nativeBarriers },
        { "pipelinesCreated", &Counters::pipelinesCreated },       { "pipelineCacheHits", &Counters::pipelineCacheHits },
        { "pipelineCacheMisses", &Counters::pipelineCacheMisses }, { "pipelineCompileNanoseconds", &Counters::pipelineCompileNanoseconds },
        { "encodersOpened", &Counters::encodersOpened },           { "deferredDestructionsRetired", &Counters::deferredDestructionsRetired },
    } };

    static_assert(sizeof(Counters) == kCounterFields.size() * sizeof(uint64_t), "every Counters field is listed");

    // `after - before`, field by field.
    inline Counters counterDelta(const Counters& after, const Counters& before)
    {
        Counters delta;
        for (const CounterField& field : kCounterFields)
        {
            delta.*field.member = after.*field.member - before.*field.member;
        }
        return delta;
    }
} // namespace rojoRHI::bench
