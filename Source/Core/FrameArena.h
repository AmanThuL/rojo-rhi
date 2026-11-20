//----------------------------------------------------------------------------------------------------------------------
/// @file FrameArena.h
/// @brief Declares the per-frame-slot arena whose pages command lists claim for `CommandList::uploadFrameData`.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace rojoRHI::core {
    // A request larger than a standard page gets a page of its own size.
    constexpr uint64_t kFrameArenaPageBytes = 1ull << 20;

    struct ArenaPage
    {
        Buffer buffer;
        std::byte* host = nullptr; // Persistently mapped.
        uint64_t size = 0;         // Bytes.
        // Read once when the page is created; zero on a backend whose addresses are no GPU addresses.
        uint64_t address = 0;
    };

    // Creates the page in the device's buffer pool, labelled as a frame-arena page.
    using ArenaPageSource = Result<ArenaPage> (*)(void* context, uint64_t size);

    // A slot owns pages that a command list claims whole, so a list bumps a cursor in its page with no lock. Only
    // claiming takes the arena's lock, and creating a page, a growth event, takes the buffer pool's lock inside it
    // (ADR 0041). Lists of one frame claim concurrently. Pages keep their memory and become claimable again when the
    // slot is reused.
    class FrameArena
    {
      public:
        explicit FrameArena(uint32_t slots);

        // The new page has `max(kFrameArenaPageBytes, minSize)` bytes, or `minSize` when the former exceeds
        // `maxPageBytes`. A request above `maxPageBytes`, or a page `source` cannot create, is fatal in every build:
        // the reference makes failing to grow fatal.
        ArenaPage claim(uint32_t slot, uint64_t minSize, uint64_t maxPageBytes, ArenaPageSource source, void* context);

        // The pages stay allocated for the next frame that uses the slot.
        void rewind(uint32_t slot);

        // Forgets every page, so the device can destroy their buffers.
        std::vector<Buffer> takePages();

        // In creation order per slot.
        std::vector<Buffer> pageBuffers() const;

        // Takes no lock.
        uint64_t growths() const;

      private:
        struct Entry
        {
            ArenaPage page;
            bool claimed = false;
        };

        // Every entry before `firstUnclaimed` is claimed, so a claim starts its search there: the j-th claim of a frame
        // whose pages are all standard costs what the first does.
        struct Slot
        {
            std::vector<Entry> entries;
            size_t firstUnclaimed = 0;
        };

        mutable std::mutex m_mutex;
        std::vector<Slot> m_slots;
        std::atomic<uint64_t> m_growths = 0;
    };
} // namespace rojoRHI::core
