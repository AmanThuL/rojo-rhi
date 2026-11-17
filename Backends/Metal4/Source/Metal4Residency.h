//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Residency.h
/// @brief Declares the device residency set: the allocations every queue of a Metal 4 device keeps resident.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <Metal/Metal.hpp>

#include <atomic>
#include <mutex>

namespace rojoRHI::metal4 {
    // One set per device, attached to every queue. Allocations join and leave it from any thread under its mutex and
    // mark it dirty; a submission commits it when dirty. The caller counts `nativeHostCalls` when `commitIfDirty`
    // returns true.
    class Residency
    {
      public:
        // The caller holds an autorelease pool.
        Result<void> create(MTL::Device* device);

        void attach(MTL4::CommandQueue* queue);

        void detach(MTL4::CommandQueue* queue);

        void add(const MTL::Allocation* allocation);

        void remove(const MTL::Allocation* allocation);

        bool contains(const MTL::Allocation* allocation);

        // Opens its own autorelease pool. A clean set is seen with one atomic load and no lock.
        bool commitIfDirty();

        // At teardown, after every queue is detached; the caller holds an autorelease pool.
        void release();

      private:
        std::mutex m_mutex;
        MTL::ResidencySet* m_set = nullptr;
        std::atomic<bool> m_dirty = false; // Set under `m_mutex`; read first without it.
    };
} // namespace rojoRHI::metal4
