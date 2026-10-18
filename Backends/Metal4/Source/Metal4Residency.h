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
    /// The one residency set of a device, attached to every queue of the device. Allocations join and leave it from any thread
    /// under its mutex and mark it dirty; a submission commits it when it is dirty. The dirty flag is atomic, so a submission
    /// that finds the set clean takes no lock. The caller counts `nativeHostCalls` when `commitIfDirty` returns true.
    class Residency
    {
      public:
        /// Creates the set on `device`, labelled `rojorhi.residency`. The caller holds an autorelease pool.
        Result<void> create(MTL::Device* device);

        /// Makes `queue` keep the set's committed allocations resident.
        void attach(MTL4::CommandQueue* queue);

        /// Detaches the set from `queue`, at teardown.
        void detach(MTL4::CommandQueue* queue);

        /// Adds `allocation` to the set and marks it dirty.
        void add(const MTL::Allocation* allocation);

        /// Removes `allocation` from the set and marks it dirty.
        void remove(const MTL::Allocation* allocation);

        /// Returns true when the set holds `allocation`.
        bool contains(const MTL::Allocation* allocation);

        /// Commits the set, inside an autorelease pool of its own, when an allocation joined or left it since the last commit;
        /// returns true when it committed. A clean set is seen with one atomic load and no lock.
        bool commitIfDirty();

        /// Releases the set, at teardown, after every queue is detached; the caller holds an autorelease pool.
        void release();

      private:
        std::mutex m_mutex;
        MTL::ResidencySet* m_set = nullptr;
        std::atomic<bool> m_dirty = false; ///< Set under `m_mutex`; read first without it.
    };
} // namespace rojoRHI::metal4
