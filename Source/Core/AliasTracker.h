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
    // A placed resource aliases from when its heap range overlaps a range that lives or lived there until it is destroyed, so
    // the first use of memory an earlier resource held is always seen; a sparse resource always counts. Placement and
    // destruction lock; the reads a backend makes while recording take none.
    class AliasTracker
    {
      public:
        void placeBuffer(Buffer buffer, Heap heap, uint64_t offset, uint64_t size);

        void placeTexture(Texture texture, Heap heap, uint64_t offset, uint64_t size);

        void unplace(Heap heap, uint64_t bits);

        void addSparse();

        void removeSparse();

        void forgetHeap(Heap heap);

        bool anyAliasing() const;

        bool anySparse() const;

        bool textureAliases(Texture texture) const;

      private:
        struct Placed
        {
            uint64_t begin = 0;
            uint64_t end = 0;
            uint32_t slot = 0; // Its texture pool slot; zero for a buffer.
            bool aliased = false;
        };

        // Every range ever placed, merged into disjoint ranges so the history stays bounded by the heap's size.
        struct HeapState
        {
            std::map<uint64_t, uint64_t> occupied;
            std::unordered_map<uint64_t, Placed> live;
        };

        static constexpr uint64_t kAliasedOne = 1;
        static constexpr uint64_t kSparseOne = uint64_t{ 1 } << 32;

        void place(uint64_t bits, uint32_t slot, Heap heap, uint64_t offset, uint64_t size);
        void markAliased(Placed& placed);

        std::mutex m_mutex;
        std::unordered_map<uint64_t, HeapState> m_heaps;
        // Live aliased resources in the low 32 bits, live Sparse ones in the high 32, in one word so `anyAliasing` is one relaxed
        // load.
        std::atomic<uint64_t> m_counts = 0;
        NativeTable m_textureFlags; // Non-null at the slot of each live placed texture that aliases.
    };
} // namespace rojoRHI::core
