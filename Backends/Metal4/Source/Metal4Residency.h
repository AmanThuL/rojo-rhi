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
    // count a change; a submission commits the set when it has changes. The caller counts `nativeHostCalls` when
    // `commitIfDirty` returns true.
    //
    // A frame-arena page joins through `addDeferred` instead, which takes no lock and makes no Metal call, because a
    // page is claimed while a list records (ADR 0041) and `commitIfDirty` holds the mutex across `commit`. The page
    // waits on a lock-free pending list that every commit drains first, so it is resident before the submission that
    // first uses it commits its command buffers.
    class Residency
    {
      public:
        // The link of one deferred add. It belongs to the allocation's owner and must outlive the allocation's `remove`,
        // which is when the list is guaranteed not to hold it any more.
        struct Pending
        {
            const MTL::Allocation* allocation = nullptr;
            Pending* next = nullptr;
        };

        // The caller holds an autorelease pool.
        Result<void> create(MTL::Device* device);

        void attach(MTL4::CommandQueue* queue);

        void detach(MTL4::CommandQueue* queue);

        void add(const MTL::Allocation* allocation);

        // Lock-free, and no Metal call: pushes `node` on the pending list and marks the set changed. The allocation joins the
        // set at the next commit of any queue or of the stager, on whatever thread commits. `remove` of the same allocation
        // before then still joins it first, so the node is never read after `remove` returns. At most once per allocation,
        // and `contains` does not see the allocation until it has joined.
        void addDeferred(const MTL::Allocation* allocation, Pending& node);

        // Joins the pending allocations first, so none outlives the owner's node.
        void remove(const MTL::Allocation* allocation);

        bool contains(const MTL::Allocation* allocation);

        // Opens its own autorelease pool. A clean set is seen with two atomic loads and no lock. A set with changes, or
        // whose commit another thread is making, is committed or waited for under the mutex, and the pending list is
        // joined to it before `commit`.
        bool commitIfDirty();

        // At teardown, after every queue is detached; the caller holds an autorelease pool.
        void release();

      private:
        // Under `m_mutex`: moves the pending list into the set.
        void joinPending();

        std::mutex m_mutex;
        MTL::ResidencySet* m_set = nullptr;
        // Every change bumps `m_changes`, after its allocation is in the set or on the pending list. A commit reads it first,
        // joins the pending list, commits, and then publishes what it read as `m_committed`. The set is clean when the two
        // are equal, so a change racing a commit leaves them apart and no change is lost, and a commit still in flight
        // leaves them apart too: a submission never skips one that has not finished.
        std::atomic<uint64_t> m_changes = 0;
        std::atomic<uint64_t> m_committed = 0; // Written under `m_mutex`.
        std::atomic<Pending*> m_pending = nullptr;
    };
} // namespace rojoRHI::metal4
