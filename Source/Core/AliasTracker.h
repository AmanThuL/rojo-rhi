//----------------------------------------------------------------------------------------------------------------------
/// @file AliasTracker.h
/// @brief Declares the tracker of which placed resources share memory, which a backend reads to order the first use of aliased memory.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/NativeTable.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <unordered_map>

namespace rojoRHI::core {
    /// Tells which live resources can share memory with another. A placed resource aliases from the moment its byte range in a
    /// heap overlaps the range of a resource that lives or lived there, and it stays aliased until it is destroyed, so the first
    /// use of memory that an earlier resource held is always seen. A sparse resource always counts, since its mapped tiles can
    /// name memory any placed resource holds. Placement and destruction take a lock; the reads a backend makes while recording
    /// take none.
    class AliasTracker
    {
      public:
        /// Notes that the buffer `buffer`, `size` bytes, was placed at `offset` in `heap`.
        void placeBuffer(Buffer buffer, Heap heap, uint64_t offset, uint64_t size);

        /// Notes that the texture `texture`, `size` bytes, was placed at `offset` in `heap`.
        void placeTexture(Texture texture, Heap heap, uint64_t offset, uint64_t size);

        /// Notes that the resource whose handle bits are `bits`, placed in `heap`, was destroyed.
        void unplace(Heap heap, uint64_t bits);

        /// Notes that a Sparse resource was created.
        void addSparse();

        /// Notes that a Sparse resource was destroyed.
        void removeSparse();

        /// Drops what is remembered of `heap`, which was destroyed.
        void forgetHeap(Heap heap);

        /// Returns true while a live resource aliases or a live resource is Sparse. Lock-free.
        bool anyAliasing() const;

        /// Returns true while a live resource is Sparse. Lock-free.
        bool anySparse() const;

        /// Returns true when the live placed texture `texture` aliases. Lock-free.
        bool textureAliases(Texture texture) const;

      private:
        /// One live placed resource.
        struct Placed
        {
            uint64_t begin = 0;   ///< The first byte of its range in the heap.
            uint64_t end = 0;     ///< One past its last byte.
            uint32_t slot = 0;    ///< The slot of its handle in the texture pool; zero for a buffer.
            bool aliased = false; ///< True once its range overlapped another's.
        };

        /// What one heap remembers: every byte range ever placed in it, merged into disjoint ranges so the history is bounded by
        /// the heap's size, and its live resources.
        struct HeapState
        {
            std::map<uint64_t, uint64_t> occupied;     ///< Disjoint ranges by first byte to one past the last.
            std::unordered_map<uint64_t, Placed> live; ///< The live resources by handle bits.
        };

        void place(uint64_t bits, uint32_t slot, Heap heap, uint64_t offset, uint64_t size);
        void markAliased(Placed& placed);

        std::mutex m_mutex;
        std::unordered_map<uint64_t, HeapState> m_heaps; ///< By heap handle bits.
        std::atomic<uint32_t> m_aliased = 0;
        std::atomic<uint32_t> m_sparse = 0;
        NativeTable m_textureFlags; ///< Non-null at the slot of each live placed texture that aliases.
    };
} // namespace rojoRHI::core
