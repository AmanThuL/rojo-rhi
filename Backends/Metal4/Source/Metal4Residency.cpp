//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Residency.cpp
/// @brief Implements the device residency set.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Residency.h"

#include <rojoRHI/RHI.h>

#include "Metal4Common.h"

#include <Metal/Metal.hpp>

#include <atomic>
#include <format>
#include <mutex>

namespace rojoRHI::metal4 {
    Result<void> Residency::create(MTL::Device* device)
    {
        MTL::ResidencySetDescriptor* descriptor = MTL::ResidencySetDescriptor::alloc()->init();
        descriptor->setLabel(makeString("rojorhi.residency"));
        NS::Error* error = nullptr;
        m_set = device->newResidencySet(descriptor, &error);
        descriptor->release();
        if (m_set == nullptr)
        {
            return std::unexpected(Error{ ErrorCategory::DriverFailure, std::format("the residency set was not created: {}", describe(error)) });
        }
        return {};
    }

    void Residency::attach(MTL4::CommandQueue* queue)
    {
        queue->addResidencySet(m_set);
    }

    void Residency::detach(MTL4::CommandQueue* queue)
    {
        queue->removeResidencySet(m_set);
    }

    void Residency::add(const MTL::Allocation* allocation)
    {
        std::lock_guard lock(m_mutex);
        m_set->addAllocation(allocation);
        m_changes.fetch_add(1, std::memory_order_release);
    }

    void Residency::addDeferred(const MTL::Allocation* allocation, Pending& node)
    {
        node.allocation = allocation;
        node.next = m_pending.load(std::memory_order_relaxed);
        // The release makes the node's fields visible to the drain's acquire exchange.
        while (!m_pending.compare_exchange_weak(node.next, &node, std::memory_order_release, std::memory_order_relaxed))
        {
        }
        // After the push: a commit that reads this count has the node on the list or already drained.
        m_changes.fetch_add(1, std::memory_order_release);
    }

    void Residency::joinPending()
    {
        // The exchange takes every node at once, so a node is joined once and a push racing it lands on the next list.
        for (Pending* node = m_pending.exchange(nullptr, std::memory_order_acquire); node != nullptr; node = node->next)
        {
            m_set->addAllocation(node->allocation);
        }
    }

    void Residency::remove(const MTL::Allocation* allocation)
    {
        std::lock_guard lock(m_mutex);
        // A page retired before any commit joins first, so its owner may free the node as soon as this returns.
        joinPending();
        m_set->removeAllocation(allocation);
        m_changes.fetch_add(1, std::memory_order_release);
    }

    bool Residency::contains(const MTL::Allocation* allocation)
    {
        std::lock_guard lock(m_mutex);
        return m_set->containsAllocation(allocation);
    }

    bool Residency::commitIfDirty()
    {
        // A change made before this call is seen here: it bumped `m_changes` before it, and `m_committed` reaches that
        // count only through a commit that joined it. One racing this call is committed by this call or by the next.
        const uint64_t changes = m_changes.load(std::memory_order_acquire);
        if (m_committed.load(std::memory_order_acquire) == changes)
        {
            return false;
        }
        std::lock_guard lock(m_mutex);
        // The count is read under the mutex and before the join: a change counted by `seen` pushed its node before it, so
        // the join below takes it.
        const uint64_t seen = m_changes.load(std::memory_order_acquire);
        if (m_committed.load(std::memory_order_relaxed) == seen)
        {
            return false;
        }
        // The commit autoreleases, and a submission holds no pool of its own.
        const AutoreleasePool pool;
        joinPending();
        m_set->commit();
        m_committed.store(seen, std::memory_order_release);
        return true;
    }

    void Residency::release()
    {
        std::lock_guard lock(m_mutex);
        if (m_set != nullptr)
        {
            m_set->release();
            m_set = nullptr;
        }
        // No queue is attached, so what is pending has nowhere to go; its owners free their nodes without it.
        m_pending.store(nullptr, std::memory_order_relaxed);
        m_committed.store(m_changes.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
} // namespace rojoRHI::metal4
