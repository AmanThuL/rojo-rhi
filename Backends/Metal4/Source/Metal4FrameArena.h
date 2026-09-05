//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4FrameArena.h
/// @brief Declares one frame slot's growable arena of mapped Metal buffers for per-frame data.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include "Core/Align.h"
#include "Core/Assert.h"
#include "Metal4Common.h"
#include "RHI/Metal4/Metal4FrameData.h"
#include "RHI/Result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lmx::rhi::metal4 {

// Where one suballocated block landed: everything bindFrameData needs to copy the bytes, publish
// the address, and name the range in a capture, all resolved by the allocation itself so the hot
// path queries nothing back out of Metal.
struct Metal4FrameDataBlock {
    uint8_t* cpu = nullptr;
    uint64_t gpuAddress = 0;
    uint32_t pageIndex = 0;
    uint64_t pageOffset = 0;
};

// The device-lifetime tallies the frame-data path keeps for tests and captures; see
// RHI/Metal4/Metal4FrameData.h for what a reader may conclude from them. Owned by Metal4Device and
// written by Metal4CommandList, which is why it is a plain struct rather than arena state: the
// numbers span every slot, while an arena knows only its own.
struct Metal4FrameDataTally {
    uint64_t calls = 0;
    uint64_t bytes = 0;
    uint64_t addressBinds = 0;
};

// One frame-in-flight slot's per-frame data arena: a growable list of persistently mapped shared
// buffers that bindFrameData bump-allocates out of.
//
// The recycle rule is the one the command allocator and the argument table both stand on: the GPU
// reads a frame's blocks for as long as that frame is in flight, so reset() is legal only once the
// pacing event has retired the slot's previous owner. Metal4Device::beginFrame proves that before
// it calls reset(); the arena holds no pacing state and cannot check it itself.
//
// Pages are never released before device destruction and the list never shrinks. A frame needing
// more than the slot owns adds a page, and every later frame on that slot reuses it -- which is
// what turns a transient peak into retained capacity instead of per-frame allocation churn.
class Metal4FrameArena {
public:
    // Creates the slot's first normal page. `device` and `residency` are the device's own, `slot`
    // names the frame-in-flight slot and appears in every page label of this arena.
    Result<void> create(MTL::Device* device, NS::SharedPtr<MTL::ResidencySet> residency,
                        uint32_t slot);

    // Suballocates `size` bytes at a GPU address that is a multiple of `alignment`, adding a page
    // when no page the slot owns can fit them. Both arguments have already been validated by
    // validateFrameData. Failing to add a page is fatal: it is device-resource exhaustion, and
    // command recording has no partial-frame failure to report through.
    //
    // Defined here, not in the .cpp, so the case every draw of a warmed-up frame takes -- the
    // active page still has room -- inlines into bindFrameData instead of crossing a translation
    // unit. Everything past that case lives in allocateGrown(), which stays out of line.
    Metal4FrameDataBlock allocate(uint64_t size, uint64_t alignment) {
        LMX_ASSERT(!m_pages.empty(),
                   "frame arena: allocate before the slot's first page was created");
        // Aligning the address rather than the offset keeps the contract the caller was given --
        // the returned GPU address is a multiple of `alignment` -- true whatever a page base is.
        Page& page = m_pages[m_activePage];
        const uint64_t mask = alignment - 1;
        const uint64_t addressRemainder = ((page.gpuBase & mask) + (page.cursor & mask)) & mask;
        const uint64_t padding = (alignment - addressRemainder) & (alignment - 1);
        if (padding <= page.capacity - page.cursor &&
            size <= page.capacity - page.cursor - padding) {
            const uint64_t offset = page.cursor + padding;
            page.cursor = offset + size;
            return {.cpu = page.cpuBase + offset,
                    .gpuAddress = page.gpuBase + offset,
                    .pageIndex = m_activePage,
                    .pageOffset = offset};
        }
        return allocateGrown(size, alignment);
    }

    // Rewinds every page's cursor. Legal only under the recycle rule above.
    void reset();

    // The label of the page an allocation named, borrowed for the arena's lifetime. Read only when
    // a capture is recording, which is what keeps the label lookup off the hot path.
    std::string_view pageLabel(uint32_t pageIndex) const;

    // Drops the pages' capture-schema identities. Called from ~Metal4Device before the buffers are
    // released: the registry keys resources by raw pointer identity, and releasing first would risk
    // a later allocation reusing that pointer value while a stale entry still described it.
    void unregisterFromCapture();

    // Releases the slot's pages and its reference to the device residency set, leaving an empty
    // arena. The device owns every arena, so it -- not a destructor here -- decides when the pages
    // go, and it calls this from inside its destructor's pool so the buffers deallocate with a
    // pool in place (destructor rule in Metal4Common.h). Not part of the recycle path: reset()
    // rewinds cursors and keeps the pages, which is the whole point of the arena.
    void release();

    FrameDataSlotCounters counters() const;

    uint32_t pageCreations() const { return m_pageCreations; }

    // What the current or most recent frame consumed, which is the diagnostic the beginFrame
    // recycle assert reports alongside the slot it is about to reset.
    uint64_t bytesUsed() const;
    uint32_t pagesUsed() const;

private:
    // One mapped buffer. The CPU base, the GPU base and the capacity are cached at creation
    // because the spec's hot path forbids re-querying any of them per call.
    struct Page {
        NS::SharedPtr<MTL::Buffer> buffer;
        uint8_t* cpuBase = nullptr;
        uint64_t gpuBase = 0;
        uint64_t capacity = 0;
        uint64_t cursor = 0;
        std::string label;
    };

    Result<void> addPage(uint64_t requestedBytes);

    // allocate()'s uncommon cases: a later page the slot already owns fits the block, or the slot
    // has to grow. Out of line on purpose -- it runs at most once per page per frame.
    Metal4FrameDataBlock allocateGrown(uint64_t size, uint64_t alignment);

    MTL::Device* m_device = nullptr;
    NS::SharedPtr<MTL::ResidencySet> m_residency;
    uint32_t m_slot = 0;
    std::vector<Page> m_pages;
    // The page allocate() bumps into. It only ever moves forward within a frame, so whatever tail
    // a page it has passed still had stays unused until the next reset -- bump allocation, not a
    // free list, which is what makes a frame's page and offset assignment reproducible.
    uint32_t m_activePage = 0;
    uint32_t m_pageCreations = 0;
};

} // namespace lmx::rhi::metal4
