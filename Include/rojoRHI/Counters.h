//----------------------------------------------------------------------------------------------------------------------
/// @file Counters.h
/// @brief Declares the counters of native work a device keeps for measurement.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// Plain integer counters of the work a device does, always compiled in and each maintained by a single increment. They
    /// count from device creation and never reset; a caller measures an interval by subtracting two snapshots from
    /// `Device::counters`. Every field means the same on every backend.
    struct Counters
    {
        uint64_t nativeCreateCalls = 0; ///< Native calls that create or release objects, views and memory.
        uint64_t nativeRecordCalls = 0; ///< Native calls recorded into command buffers: state, draws, dispatches, copies, barriers.
        uint64_t nativeSubmitCalls = 0; ///< Native queue calls: submission, presentation, tile mapping, fence signals and waits.
        uint64_t nativeHostCalls = 0;   ///< Other native calls on the CPU: residency commits, fence queries, timestamp reads.
        /// CPU heap allocations RojoRHI makes at explicit sites: each `new`, `make_unique` and host memory block the library
        /// allocates itself. Growth inside standard containers is not counted, and no allocator is replaced to count it.
        uint64_t cpuAllocations = 0;
        uint64_t gpuAllocations = 0;              ///< Native memory objects created: buffers, textures, heaps and frame-arena pages.
        uint64_t frameDataUploads = 0;            ///< `CommandList::uploadFrameData` calls.
        uint64_t frameArenaBytes = 0;             ///< Bytes those calls copied, excluding alignment padding.
        uint64_t frameArenaGrowths = 0;           ///< Frame-arena growth events.
        uint64_t descriptorWrites = 0;            ///< Heap-slot writes.
        uint64_t globalBarriers = 0;              ///< Global barrier records passed to `CommandList::barrier`.
        uint64_t bufferBarriers = 0;              ///< Buffer barrier records passed to `CommandList::barrier`.
        uint64_t textureBarriers = 0;             ///< Texture barrier records passed to `CommandList::barrier`.
        uint64_t nativeBarriers = 0;              ///< Native barrier commands emitted after batching.
        uint64_t pipelinesCreated = 0;            ///< Pipelines created, including any variant a backend creates on its own.
        uint64_t pipelineCacheHits = 0;           ///< Pipeline creations served by the pipeline cache.
        uint64_t pipelineCacheMisses = 0;         ///< Pipeline creations not served by the pipeline cache.
        uint64_t pipelineCompileNanoseconds = 0;  ///< CPU time spent in pipeline creation; the one timed field, not exact.
        uint64_t encodersOpened = 0;              ///< Native encoders on Metal 4; render passes begun elsewhere.
        uint64_t deferredDestructionsRetired = 0; ///< Deferred destructions whose native release has run.
    };
} // namespace rojoRHI
