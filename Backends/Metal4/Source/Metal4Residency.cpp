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
    //==================================================================================================================
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

    //==================================================================================================================
    void Residency::attach(MTL4::CommandQueue* queue)
    {
        queue->addResidencySet(m_set);
    }

    //==================================================================================================================
    void Residency::detach(MTL4::CommandQueue* queue)
    {
        queue->removeResidencySet(m_set);
    }

    //==================================================================================================================
    void Residency::add(const MTL::Allocation* allocation)
    {
        std::lock_guard lock(m_mutex);
        m_set->addAllocation(allocation);
        m_dirty.store(true, std::memory_order_release);
    }

    //==================================================================================================================
    void Residency::remove(const MTL::Allocation* allocation)
    {
        std::lock_guard lock(m_mutex);
        m_set->removeAllocation(allocation);
        m_dirty.store(true, std::memory_order_release);
    }

    //==================================================================================================================
    bool Residency::contains(const MTL::Allocation* allocation)
    {
        std::lock_guard lock(m_mutex);
        return m_set->containsAllocation(allocation);
    }

    //==================================================================================================================
    bool Residency::commitIfDirty()
    {
        // A change made before this call is seen here; one racing it is committed by this call or by the next.
        if (!m_dirty.load(std::memory_order_acquire))
        {
            return false;
        }
        std::lock_guard lock(m_mutex);
        if (!m_dirty.load(std::memory_order_relaxed))
        {
            return false;
        }
        // The commit autoreleases, and a submission holds no pool of its own.
        const AutoreleasePool pool;
        m_set->commit();
        m_dirty.store(false, std::memory_order_relaxed);
        return true;
    }

    //==================================================================================================================
    void Residency::release()
    {
        std::lock_guard lock(m_mutex);
        if (m_set != nullptr)
        {
            m_set->release();
            m_set = nullptr;
        }
        m_dirty.store(false, std::memory_order_relaxed);
    }
} // namespace rojoRHI::metal4
