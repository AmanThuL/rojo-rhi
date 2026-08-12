//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4FrameArena.cpp
/// @brief Implements per-frame data page allocation, growth, reuse, and instrumentation.
//----------------------------------------------------------------------------------------------------------------------
#include "Metal4FrameArena.h"

#include "Core/Align.h"
#include "Core/Assert.h"
#include "Metal4Device.h"
#include "RHI/CaptureSchema.h"

#include <format>
#include <limits>

namespace lmx::rhi::metal4 {

//======================================================================================================================
Result<void> Metal4FrameArena::create(MTL::Device* device,
                                      NS::SharedPtr<MTL::ResidencySet> residency, uint32_t slot) {
    LMX_ASSERT(device != nullptr, "frame arena: device must not be null");
    LMX_ASSERT(residency, "frame arena: residency set must not be null");
    m_device = device;
    m_residency = std::move(residency);
    m_slot = slot;
    return addPage(kFrameDataPageBytes);
}

//======================================================================================================================
// Shared storage under the default CPU cache mode. Write-combined looks like the obvious fit --
// the CPU only writes these pages and the GPU only reads them -- but on Apple silicon that hint
// maps the range non-cacheable, and bindFrameData's writes are small blocks scattered across a
// caller-chosen alignment stride rather than one long streaming run. Each block then leaves the
// core as its own uncached transaction instead of retiring into L1: measured at 42.6 ns per call
// on F-FIT-512 (M3 Max), which was the whole of that workload's regression against the fixed ring
// this arena replaced. Cached pages cost the GPU nothing here -- shared storage is coherent on
// this hardware either way.
Result<void> Metal4FrameArena::addPage(uint64_t requestedBytes) {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    if (requestedBytes > std::numeric_limits<uint64_t>::max() - (kFrameDataPageBytes - 1)) {
        return std::unexpected(
            Error{ErrorCode::ResourceCreationFailed,
                  std::format("frame-data page request of {} bytes overflows the {}-byte page "
                              "quantum for frame slot {}",
                              requestedBytes, kFrameDataPageBytes, m_slot)});
    }
    const uint64_t capacity =
        alignUp(requestedBytes < kFrameDataPageBytes ? kFrameDataPageBytes : requestedBytes,
                kFrameDataPageBytes);

    NS::SharedPtr<MTL::Buffer> buffer =
        NS::TransferPtr(m_device->newBuffer(capacity, MTL::ResourceStorageModeShared));
    if (!buffer) {
        return std::unexpected(
            Error{ErrorCode::ResourceCreationFailed,
                  std::format("failed to create frame-data page {} of {} bytes for frame slot {}",
                              m_pages.size(), capacity, m_slot)});
    }

    Page page;
    page.label = std::format("lmx.device.frameData.{}.page.{}", m_slot, m_pages.size());
    buffer->setLabel(makeString(page.label).get());
    page.cpuBase = static_cast<uint8_t*>(buffer->contents());
    page.gpuBase = buffer->gpuAddress();
    page.capacity = capacity;
    LMX_ASSERT(page.cpuBase != nullptr && page.gpuBase != 0,
               "frame arena: a shared page came back without a mapped CPU or GPU base");
    // Every block is placed at least at kFrameDataAlignment, and a fresh page's cursor is zero, so
    // a page base coarser than that is what lets offset zero satisfy the first request.
    LMX_ASSERT(page.gpuBase % kFrameDataAlignment == 0,
               "frame arena: Metal returned a page whose GPU base is not constant-buffer aligned");

    // Device-owned pages bypass the resource wrappers, so their capture identity is registered
    // here directly.
    debug::CaptureSchema::instance().registerBuffer(buffer.get(), page.label, capacity);
    // The set is attached to the queue, and a commit republishes the whole of it. A page added
    // mid-frame therefore becomes resident before the frame's command buffer is committed, which
    // is the only ordering the GPU cares about here.
    m_residency->addAllocation(buffer.get());
    m_residency->commit();

    page.buffer = std::move(buffer);
    m_pages.push_back(std::move(page));
    ++m_pageCreations;
    return {};
}

//======================================================================================================================
// Reached only when the active page cannot fit the block: allocate()'s inline fast path in the
// header has already tried it and failed, so the scan resumes at the next page.
Metal4FrameDataBlock Metal4FrameArena::allocateGrown(uint64_t size, uint64_t alignment) {
    // Aligning the address rather than the offset keeps the contract the caller was given -- the
    // returned GPU address is a multiple of `alignment` -- true whatever a page base happens to be.
    for (uint32_t index = m_activePage + 1; index < m_pages.size(); ++index) {
        Page& page = m_pages[index];
        const uint64_t mask = alignment - 1;
        const uint64_t addressRemainder = ((page.gpuBase & mask) + (page.cursor & mask)) & mask;
        const uint64_t padding = (alignment - addressRemainder) & (alignment - 1);
        if (padding <= page.capacity - page.cursor &&
            size <= page.capacity - page.cursor - padding) {
            const uint64_t offset = page.cursor + padding;
            page.cursor = offset + size;
            m_activePage = index;
            return {.cpu = page.cpuBase + offset,
                    .gpuAddress = page.gpuBase + offset,
                    .pageIndex = index,
                    .pageOffset = offset};
        }
    }

    const uint32_t grown = static_cast<uint32_t>(m_pages.size());
    // A fresh Metal allocation is guaranteed only the RHI's default alignment. Reserve the
    // worst-case leading padding so every alignment accepted by validateFrameData fits whatever
    // GPU base Metal returns.
    LMX_ASSERT(size <= std::numeric_limits<uint64_t>::max() - (alignment - 1),
               "bindFrameData: validated size and alignment overflowed page capacity");
    const Result<void> page = addPage(size + alignment - 1);
    LMX_ASSERT(page.has_value(),
               std::format("bindFrameData: frame slot {} cannot grow to fit a {}-byte block; its "
                           "{} page(s) already hold {} bytes of capacity -- {}",
                           m_slot, size, grown, counters().capacityBytes, page.error().message));
    m_activePage = grown;

    Page& fresh = m_pages[grown];
    const uint64_t remainder = fresh.gpuBase & (alignment - 1);
    const uint64_t offset = (alignment - remainder) & (alignment - 1);
    LMX_ASSERT(offset <= fresh.capacity && size <= fresh.capacity - offset,
               std::format("bindFrameData: a fresh {}-byte page of frame slot {} still cannot hold "
                           "a {}-byte block aligned to {} bytes",
                           fresh.capacity, m_slot, size, alignment));
    fresh.cursor = offset + size;
    return {.cpu = fresh.cpuBase + offset,
            .gpuAddress = fresh.gpuBase + offset,
            .pageIndex = grown,
            .pageOffset = offset};
}

//======================================================================================================================
void Metal4FrameArena::reset() {
    for (Page& page : m_pages) {
        page.cursor = 0;
    }
    m_activePage = 0;
}

//======================================================================================================================
std::string_view Metal4FrameArena::pageLabel(uint32_t pageIndex) const {
    LMX_ASSERT(pageIndex < m_pages.size(), "frame arena: page label requested for a missing page");
    return m_pages[pageIndex].label;
}

//======================================================================================================================
void Metal4FrameArena::unregisterFromCapture() {
    for (const Page& page : m_pages) {
        if (page.buffer) {
            debug::CaptureSchema::instance().unregisterResource(page.buffer.get());
        }
    }
}

//======================================================================================================================
uint64_t Metal4FrameArena::bytesUsed() const {
    uint64_t used = 0;
    for (const Page& page : m_pages) {
        used += page.cursor;
    }
    return used;
}

//======================================================================================================================
// Counted from the active page rather than from how many pages hold bytes: a request the active
// page could not fit skips past it, so a skipped page with a zero cursor was still consumed by the
// frame in the only sense that matters -- nothing later will allocate out of it.
uint32_t Metal4FrameArena::pagesUsed() const {
    return bytesUsed() == 0 ? 0 : m_activePage + 1;
}

//======================================================================================================================
FrameDataSlotCounters Metal4FrameArena::counters() const {
    FrameDataSlotCounters slot;
    slot.pageCount = static_cast<uint32_t>(m_pages.size());
    slot.pagesUsed = pagesUsed();
    slot.bytesUsed = bytesUsed();
    for (const Page& page : m_pages) {
        slot.capacityBytes += page.capacity;
    }
    return slot;
}

//======================================================================================================================
FrameDataCounters frameDataCounters(const Device& device) {
    return static_cast<const Metal4Device&>(device).frameDataCounters();
}

} // namespace lmx::rhi::metal4
