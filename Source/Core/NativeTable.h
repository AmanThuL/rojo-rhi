//----------------------------------------------------------------------------------------------------------------------
/// @file NativeTable.h
/// @brief Declares the lock-free table of native payloads that a handle pool keeps, one entry per slot.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Base/Assert.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace rojoRHI::core {
    // Readable without a lock. A fixed directory of `kChunks` entries points at chunks of `kChunkSlots` atomic
    // pointers, each allocated zeroed on its first publish and never moved or freed before the table is destroyed, so a
    // reader holding an index never races a reallocation. Writers are serialised by the owner; readers see a payload
    // published before the index reached them.
    class NativeTable
    {
      public:
        static constexpr uint32_t kChunkSlots = 4096;
        static constexpr uint32_t kChunks = 1024;
        static constexpr uint32_t kCapacity = kChunkSlots * kChunks; // 2^22.
        static_assert((kCapacity & (kCapacity - 1)) == 0, "load masks an index with kCapacity - 1");

        // At most `kCapacity`; a smaller value lets a test fill it.
        explicit NativeTable(uint32_t capacity = kCapacity) : m_capacity(capacity)
        {
            ROJORHI_INTERNAL_ASSERT(capacity <= kCapacity, "a native table larger than its directory");
        }

        ~NativeTable()
        {
            for (std::atomic<std::atomic<void*>*>& entry : m_directory)
            {
                delete[] entry.load(std::memory_order_relaxed);
            }
        }

        NativeTable(const NativeTable&) = delete;
        NativeTable& operator=(const NativeTable&) = delete;

        // Slot zero included.
        uint32_t capacity() const
        {
            return m_capacity;
        }

        // Release store, allocating the chunk on first use; callers serialise every publish.
        void publish(uint32_t index, void* native)
        {
            ROJORHI_INTERNAL_ASSERT(index < m_capacity, "a native payload published past the table's capacity");
            std::atomic<std::atomic<void*>*>& entry = m_directory[index / kChunkSlots];
            std::atomic<void*>* chunk = entry.load(std::memory_order_relaxed);
            if (chunk == nullptr)
            {
                chunk = new std::atomic<void*>[kChunkSlots]();
                entry.store(chunk, std::memory_order_release);
            }
            chunk[index % kChunkSlots].store(native, std::memory_order_release);
        }

        // Acquire loads, no lock, no check; null when the chunk was never allocated. An index past `kCapacity` wraps
        // into the directory rather than reading past it.
        void* load(uint32_t index) const
        {
            index &= kCapacity - 1;
            const std::atomic<void*>* chunk = m_directory[index / kChunkSlots].load(std::memory_order_acquire);
            return chunk != nullptr ? chunk[index % kChunkSlots].load(std::memory_order_acquire) : nullptr;
        }

      private:
        std::array<std::atomic<std::atomic<void*>*>, kChunks> m_directory = {};
        uint32_t m_capacity;
    };
} // namespace rojoRHI::core
