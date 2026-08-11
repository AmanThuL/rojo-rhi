//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4DeviceFrame.cpp
/// @brief Implements Metal 4 frame pacing, submission, retirement, and GPU timing publication.
//----------------------------------------------------------------------------------------------------------------------
#include "Metal4Device.h"

#include "Core/Assert.h"
#include "Metal4Swapchain.h"

#include <format>

namespace lmx::rhi::metal4 {

//======================================================================================================================
double Metal4Device::passMilliseconds(uint64_t beginTicks, uint64_t endTicks) const {
    // An entry the GPU never wrote still holds the value invalidateCounterRange left there.
    // Reporting a duration for one would be inventing a measurement.
    LMX_ASSERT(beginTicks != MTL::CounterErrorValue && endTicks != MTL::CounterErrorValue,
               "pass timing: the GPU left a timestamp of an encoded pass unwritten");
    LMX_ASSERT(endTicks >= beginTicks,
               "pass timing: a pass ended at an earlier GPU timestamp than it began");
    return static_cast<double>(endTicks - beginTicks) * 1000.0 /
           static_cast<double>(m_timestampTicksPerSecond);
}

//======================================================================================================================
void Metal4Device::resolveRetiredPassTimings() {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    // The event is the only proof that a slot's timestamps are finished being written.
    const uint64_t retired = m_frameEvent->signaledValue();

    // With three frames in flight two slots can be retired and unpublished at the same time;
    // publishing the older one would walk the readout backwards.
    const Metal4FrameTimestamps* newest = nullptr;
    for (const Metal4FrameTimestamps& candidate : m_frameTimestamps) {
        if (candidate.frameNumber <= m_resolvedFrame || candidate.frameNumber > retired) {
            continue;
        }
        if (newest == nullptr || candidate.frameNumber > newest->frameNumber) {
            newest = &candidate;
        }
    }
    if (newest == nullptr) {
        return;
    }

    m_resolvedFrame = newest->frameNumber;
    m_passTimings.clear();
    if (newest->passLabels.empty()) {
        return;
    }

    const NS::UInteger entryCount = newest->passLabels.size() * 2;
    NS::Data* resolved = newest->heap->resolveCounterRange(NS::Range::Make(0, entryCount));
    LMX_ASSERT(resolved != nullptr,
               "beginFrame: the timestamp heap of a retired frame refused to resolve");
    LMX_ASSERT(resolved->length() == entryCount * sizeof(MTL4::TimestampHeapEntry),
               "beginFrame: the resolved timestamp heap is not a plain array of heap entries");
    const auto* entries = static_cast<const MTL4::TimestampHeapEntry*>(resolved->bytes());

    m_passTimings.reserve(newest->passLabels.size());
    for (size_t pass = 0; pass < newest->passLabels.size(); ++pass) {
        m_passTimings.push_back(
            {.label = newest->passLabels[pass],
             .gpuMilliseconds =
                 passMilliseconds(entries[pass * 2].timestamp, entries[pass * 2 + 1].timestamp)});
    }
}

//======================================================================================================================
CommandList& Metal4Device::beginFrame() {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    LMX_ASSERT(!m_frameOpen, "beginFrame: the previous frame is still open -- call endFrame");

    ++m_frameNumber;

    // A slot is reusable only after the event retires the frame that previously owned it.
    if (m_frameNumber > kFramesInFlight) {
        const uint64_t completedFrame = m_frameNumber - kFramesInFlight;
        const bool signaled = m_frameEvent->waitUntilSignaledValue(completedFrame, kGpuTimeoutMs);
        LMX_ASSERT(signaled, "beginFrame: the GPU did not finish the frame that owns this "
                             "frame's command allocator within the timeout");
    }

    // Allocator, argument table, and uniform ring share the same retirement proof.
    const uint32_t slot = static_cast<uint32_t>(m_frameNumber % kFramesInFlight);

    // Check the shared-event invariant before Metal sees a premature allocator reset. Untouched
    // slots use frame zero, which is already retired by the event's initial value.
    const UniformRingUse& lastUse = m_uniformRingUse[slot];
    LMX_ASSERT(m_frameEvent->signaledValue() >= lastUse.frameNumber,
               std::format("beginFrame: frame {} is about to recycle the ring slot still holding "
                           "{} bytes of frame {}'s uniforms, but the GPU has only retired through "
                           "frame {} -- frame pacing is broken (the frame's allocator and argument "
                           "table are equally unretired)",
                           m_frameNumber, lastUse.bytesUsed, lastUse.frameNumber,
                           m_frameEvent->signaledValue()));

    // Publish before the slot is recycled below: this is the last moment the retiring frame's
    // timestamps still exist, and the event above has already proven they are readable.
    resolveRetiredPassTimings();

    MTL4::CommandAllocator* allocator = m_allocators[slot].get();
    allocator->reset();
    m_commandBuffer->beginCommandBuffer(allocator);

    // The retirement wait makes this slot's uniform bytes safe to overwrite.
    m_uniformOffsets[slot] = 0;
    Metal4FrameTimestamps& timestamps = m_frameTimestamps[slot];
    timestamps.heap->invalidateCounterRange(NS::Range::Make(0, kTimestampsPerFrame));
    timestamps.passLabels.clear();
    timestamps.frameNumber = 0;
    m_commandList->resetForFrame(m_argumentTables[slot].get(), m_uniformRings[slot].get(),
                                 &m_uniformOffsets[slot], &timestamps);

    m_frameOpen = true;
    return *m_commandList;
}

//======================================================================================================================
void Metal4Device::endFrame(Swapchain* presentTo) {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    LMX_ASSERT(m_frameOpen, "endFrame: no frame is open -- call beginFrame first");
    LMX_ASSERT(!m_commandList->inRenderPass(),
               "endFrame: a render pass is still open -- call endRenderPass first");
    LMX_ASSERT(!m_commandList->inComputePass(),
               "endFrame: a compute pass is still open -- call endComputePass first");
    LMX_ASSERT(!m_commandList->inCopyPass(),
               "endFrame: a copy pass is still open -- call endCopyPass first");
    LMX_ASSERT(!m_commandList->hasPendingBarrier(),
               "endFrame: a textureBarrier or bufferBarrier has no later consumer pass");

    m_commandBuffer->endCommandBuffer();

    auto* swapchain = static_cast<Metal4Swapchain*>(presentTo);
    CA::MetalDrawable* drawable = swapchain != nullptr ? swapchain->currentDrawable() : nullptr;
    LMX_ASSERT(swapchain == nullptr || drawable != nullptr,
               "endFrame: asked to present a swapchain whose texture was never acquired");

    if (drawable != nullptr) {
        // Wait before drawable writes; signal only after the submitted work completes.
        m_queue->wait(drawable);
    }

    const MTL4::CommandBuffer* commandBuffers[] = {m_commandBuffer.get()};
    m_queue->commit(commandBuffers, 1);

    if (drawable != nullptr) {
        m_queue->signalDrawable(drawable);
        drawable->present();
        swapchain->releaseCurrentDrawable();
    }

    // Publish retirement after all commands and drawable signaling for this frame.
    m_queue->signalEvent(m_frameEvent.get(), m_frameNumber);

    // Record ownership only after work consuming the ring has been submitted.
    const uint32_t slot = static_cast<uint32_t>(m_frameNumber % kFramesInFlight);
    m_uniformRingUse[slot] = {.frameNumber = m_frameNumber, .bytesUsed = m_uniformOffsets[slot]};
    // Claims this slot's timestamps for this frame -- only now is there submitted work that will
    // eventually make the pacing event vouch for them.
    m_frameTimestamps[slot].frameNumber = m_frameNumber;

    // Drop CPU-side frame pointers while the GPU owns the submitted slot.
    m_commandList->endFrameReset();
    m_frameOpen = false;
}

//======================================================================================================================
void Metal4Device::waitIdle() {
    drainQueue(m_queue.get());
}

} // namespace lmx::rhi::metal4
