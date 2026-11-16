//----------------------------------------------------------------------------------------------------------------------
/// @file AliasTracker.cpp
/// @brief Implements the tracker of which placed resources share memory.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/AliasTracker.h"

#include "Base/Assert.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"

#include <algorithm>

namespace rojoRHI::core {
    namespace {
        /// The value a texture's flag holds while it aliases; only its being non-null matters.
        int gAliasMark = 0;

        //==============================================================================================================
        /// Returns true when `occupied` holds a byte of [begin, end).
        bool overlaps(const std::map<uint64_t, uint64_t>& occupied, uint64_t begin, uint64_t end)
        {
            auto next = occupied.upper_bound(begin);
            if (next != occupied.begin() && std::prev(next)->second > begin)
            {
                return true;
            }
            return next != occupied.end() && next->first < end;
        }

        //==============================================================================================================
        /// Adds [begin, end) to `occupied`, merging the ranges it touches.
        void occupy(std::map<uint64_t, uint64_t>& occupied, uint64_t begin, uint64_t end)
        {
            auto next = occupied.upper_bound(begin);
            if (next != occupied.begin() && std::prev(next)->second >= begin)
            {
                --next;
                begin = next->first;
                end = std::max(end, next->second);
                next = occupied.erase(next);
            }
            while (next != occupied.end() && next->first <= end)
            {
                end = std::max(end, next->second);
                next = occupied.erase(next);
            }
            occupied.emplace(begin, end);
        }
    } // namespace

    //==================================================================================================================
    void AliasTracker::placeBuffer(Buffer buffer, Heap heap, uint64_t offset, uint64_t size)
    {
        place(buffer.bits, 0, heap, offset, size);
    }

    //==================================================================================================================
    void AliasTracker::placeTexture(Texture texture, Heap heap, uint64_t offset, uint64_t size)
    {
        place(texture.bits, HandlePool<Texture, TextureRecord>::slotIndex(texture), heap, offset, size);
    }

    //==================================================================================================================
    void AliasTracker::place(uint64_t bits, uint32_t slot, Heap heap, uint64_t offset, uint64_t size)
    {
        std::lock_guard lock(m_mutex);
        HeapState& state = m_heaps[heap.bits];
        const uint64_t end = offset + size;
        Placed placed{ offset, end, slot, false };
        if (overlaps(state.occupied, offset, end))
        {
            for (auto& [other, resource] : state.live)
            {
                if (!resource.aliased && resource.begin < end && offset < resource.end)
                {
                    markAliased(resource);
                }
            }
            markAliased(placed);
        }
        occupy(state.occupied, offset, end);
        state.live.emplace(bits, placed);
    }

    //==================================================================================================================
    void AliasTracker::markAliased(Placed& placed)
    {
        placed.aliased = true;
        m_counts.fetch_add(kAliasedOne, std::memory_order_relaxed);
        if (placed.slot != 0)
        {
            m_textureFlags.publish(placed.slot, &gAliasMark);
        }
    }

    //==================================================================================================================
    void AliasTracker::unplace(Heap heap, uint64_t bits)
    {
        std::lock_guard lock(m_mutex);
        const auto state = m_heaps.find(heap.bits);
        if (state == m_heaps.end())
        {
            return;
        }
        const auto placed = state->second.live.find(bits);
        if (placed == state->second.live.end())
        {
            return;
        }
        if (placed->second.aliased)
        {
            [[maybe_unused]] const uint64_t before = m_counts.fetch_sub(kAliasedOne, std::memory_order_relaxed);
            ROJORHI_INTERNAL_ASSERT((before & 0xffffffff) != 0, "more aliased resources were destroyed than were counted");
            if (placed->second.slot != 0)
            {
                m_textureFlags.publish(placed->second.slot, nullptr);
            }
        }
        state->second.live.erase(placed);
    }

    //==================================================================================================================
    void AliasTracker::addSparse()
    {
        m_counts.fetch_add(kSparseOne, std::memory_order_relaxed);
    }

    //==================================================================================================================
    void AliasTracker::removeSparse()
    {
        [[maybe_unused]] const uint64_t before = m_counts.fetch_sub(kSparseOne, std::memory_order_relaxed);
        ROJORHI_INTERNAL_ASSERT(before >= kSparseOne, "more sparse resources were destroyed than were created");
    }

    //==================================================================================================================
    void AliasTracker::forgetHeap(Heap heap)
    {
        std::lock_guard lock(m_mutex);
        m_heaps.erase(heap.bits);
    }

    //==================================================================================================================
    bool AliasTracker::anyAliasing() const
    {
        return m_counts.load(std::memory_order_relaxed) != 0;
    }

    //==================================================================================================================
    bool AliasTracker::anySparse() const
    {
        return m_counts.load(std::memory_order_relaxed) >= kSparseOne;
    }

    //==================================================================================================================
    bool AliasTracker::textureAliases(Texture texture) const
    {
        return m_textureFlags.load(HandlePool<Texture, TextureRecord>::slotIndex(texture)) != nullptr;
    }
} // namespace rojoRHI::core
