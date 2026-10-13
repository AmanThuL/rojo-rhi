//----------------------------------------------------------------------------------------------------------------------
/// @file FrameArena.h
/// @brief Declares the per-frame-slot arena whose pages command lists claim for `CommandList::uploadFrameData`.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Handles.h>
#include <rojoRHI/Result.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace rojoRHI::core {
    /// Bytes of a standard arena page; a request larger than that gets a page of its own size.
    constexpr uint64_t kFrameArenaPageBytes = 1ull << 20;

    /// One arena page: a host-visible buffer of the device's buffer pool and the pointer to its first byte.
    struct ArenaPage
    {
        Buffer buffer;             ///< The page's buffer.
        std::byte* host = nullptr; ///< The page's persistently mapped memory.
        uint64_t size = 0;         ///< Bytes.
    };

    /// A function that creates one arena page of `size` bytes in the device's buffer pool, labelled as a frame-arena page.
    using ArenaPageSource = Result<ArenaPage> (*)(void* context, uint64_t size);

    /// One arena per frame slot. A slot owns pages that a command list claims whole, so a list bumps a cursor in its page
    /// with no lock. Only claiming a page takes the arena's lock, and creating a page is a growth event, which takes the
    /// buffer pool's lock inside the arena's (ADR 0020). Pages keep their memory and become claimable again when the slot
    /// is reused. Claiming is thread-safe: lists of one frame claim concurrently.
    class FrameArena
    {
      public:
        /// Creates an arena with `slots` empty slots.
        explicit FrameArena(uint32_t slots);

        /// Returns an unclaimed page of `slot` of at least `minSize` bytes, creating one through `source` when none fits. The
        /// new page has `max(kFrameArenaPageBytes, minSize)` bytes, or `minSize` when the former exceeds `maxPageBytes`. A
        /// request above `maxPageBytes`, or a page `source` cannot create, is fatal in every build: the reference makes
        /// failing to grow fatal.
        ArenaPage claim(uint32_t slot, uint64_t minSize, uint64_t maxPageBytes, ArenaPageSource source, void* context);

        /// Makes every page of `slot` claimable again; the pages stay allocated for the next frame that uses the slot.
        void rewind(uint32_t slot);

        /// Returns every page of every slot and forgets them, so the device can destroy their buffers.
        std::vector<Buffer> takePages();

        /// Returns the buffers of every page, in creation order per slot.
        std::vector<Buffer> pageBuffers() const;

        /// Returns the number of pages created so far, across every slot; takes no lock.
        uint64_t growths() const;

      private:
        struct Entry
        {
            ArenaPage page;
            bool claimed = false;
        };

        mutable std::mutex m_mutex;
        std::vector<std::vector<Entry>> m_slots;
        std::atomic<uint64_t> m_growths = 0;
    };
} // namespace rojoRHI::core
