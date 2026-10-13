//----------------------------------------------------------------------------------------------------------------------
/// @file Counters.h
/// @brief Declares the bookkeeping behind `Device::counters`: the device's atomic counts and the plain counts a command list keeps.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace rojoRHI::core {
    /// The counts a command list accumulates while it records, in plain fields so recording takes no lock and no atomic
    /// operation. `submitLists` folds them into the device when the list is submitted.
    struct RecordCounts
    {
        uint64_t nativeRecordCalls = 0; ///< Recording entries whose Metal 4 mapping names a native call.
        uint64_t frameDataUploads = 0;  ///< `uploadFrameData` calls; scope labels copied into the arena are not uploads.
        uint64_t frameArenaBytes = 0;   ///< Bytes `uploadFrameData` copied, excluding alignment padding.
        uint64_t globalBarriers = 0;    ///< Global barrier records passed to `CommandList::barrier`.
        uint64_t bufferBarriers = 0;    ///< Buffer barrier records passed to `CommandList::barrier`.
        uint64_t textureBarriers = 0;   ///< Texture barrier records passed to `CommandList::barrier`.
        uint64_t nativeBarriers = 0;    ///< `CommandList::barrier` calls with at least one record, each batched into one native command.
        uint64_t encodersOpened = 0;    ///< `beginRenderPass` calls.
    };

    /// The device's counts, all incremented relaxed from any thread and read with relaxed loads, so a snapshot takes no lock.
    /// The handle pools add to the creation counts as they issue records.
    struct DeviceCounts
    {
        std::atomic<uint64_t> nativeCreateCalls = 0; ///< Native objects created or released: pool records, swapchains, releases.
        std::atomic<uint64_t> gpuAllocations = 0;    ///< Buffers, textures (swapchain images included), heaps; arena pages are buffers.
        std::atomic<uint64_t> pipelinesCreated = 0;  ///< Pipelines created.
        std::atomic<uint64_t> deferredRetired = 0;   ///< Deferred destructions that ran.
        std::atomic<uint64_t> nativeSubmitCalls = 0; ///< Submissions, presents, tile mappings, fence signals and waits.
        std::atomic<uint64_t> nativeHostCalls = 0;   ///< Fence queries and timestamp reads.
        /// Explicit heap allocation sites; growth inside standard containers is not counted and no allocator is replaced.
        std::atomic<uint64_t> cpuAllocations = 0;
        std::atomic<uint64_t> descriptorWrites = 0;           ///< Heap-slot writes.
        std::atomic<uint64_t> pipelineCompileNanoseconds = 0; ///< Time spent creating pipelines.
        std::atomic<uint64_t> nativeRecordCalls = 0;          ///< Folded from the submitted lists.
        std::atomic<uint64_t> frameDataUploads = 0;           ///< Folded from the submitted lists.
        std::atomic<uint64_t> frameArenaBytes = 0;            ///< Folded from the submitted lists.
        std::atomic<uint64_t> globalBarriers = 0;             ///< Folded from the submitted lists.
        std::atomic<uint64_t> bufferBarriers = 0;             ///< Folded from the submitted lists.
        std::atomic<uint64_t> textureBarriers = 0;            ///< Folded from the submitted lists.
        std::atomic<uint64_t> nativeBarriers = 0;             ///< Folded from the submitted lists.
        std::atomic<uint64_t> encodersOpened = 0;             ///< Folded from the submitted lists.

        /// Adds `by` to `counter` with relaxed ordering.
        static void add(std::atomic<uint64_t>& counter, uint64_t by = 1)
        {
            counter.fetch_add(by, std::memory_order_relaxed);
        }

        /// Reads `counter` with relaxed ordering.
        static uint64_t read(const std::atomic<uint64_t>& counter)
        {
            return counter.load(std::memory_order_relaxed);
        }

        /// Adds the counts of one submitted list.
        void fold(const RecordCounts& counts)
        {
            add(nativeRecordCalls, counts.nativeRecordCalls);
            add(frameDataUploads, counts.frameDataUploads);
            add(frameArenaBytes, counts.frameArenaBytes);
            add(globalBarriers, counts.globalBarriers);
            add(bufferBarriers, counts.bufferBarriers);
            add(textureBarriers, counts.textureBarriers);
            add(nativeBarriers, counts.nativeBarriers);
            add(encodersOpened, counts.encodersOpened);
        }
    };

    /// Adds the wall time between its construction and destruction, in nanoseconds, to a counter.
    class ScopedNanoseconds
    {
      public:
        /// Starts timing for `counter`.
        explicit ScopedNanoseconds(std::atomic<uint64_t>& counter) : m_counter(counter), m_start(std::chrono::steady_clock::now())
        {
        }

        ScopedNanoseconds(const ScopedNanoseconds&) = delete;
        ScopedNanoseconds& operator=(const ScopedNanoseconds&) = delete;

        /// Adds the elapsed time.
        ~ScopedNanoseconds()
        {
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - m_start);
            DeviceCounts::add(m_counter, static_cast<uint64_t>(elapsed.count()));
        }

      private:
        std::atomic<uint64_t>& m_counter;
        std::chrono::steady_clock::time_point m_start;
    };
} // namespace rojoRHI::core
