#include "RHI/Metal4/Metal4Device.h"

#include "Core/Assert.h"
#include "RHI/Metal4/Metal4Swapchain.h"

#include <format>

namespace lmx::rhi::metal4 {

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

    MTL4::CommandAllocator* allocator = m_allocators[slot].get();
    allocator->reset();
    m_commandBuffer->beginCommandBuffer(allocator);

    // The retirement wait makes this slot's uniform bytes safe to overwrite.
    m_uniformOffsets[slot] = 0;
    m_commandList->resetForFrame(m_argumentTables[slot].get(), m_uniformRings[slot].get(),
                                 &m_uniformOffsets[slot]);

    m_frameOpen = true;
    return *m_commandList;
}

//======================================================================================================================
void Metal4Device::endFrame(Swapchain* presentTo) {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    LMX_ASSERT(m_frameOpen, "endFrame: no frame is open -- call beginFrame first");
    LMX_ASSERT(!m_commandList->inRenderPass(),
               "endFrame: a render pass is still open -- call endRenderPass first");

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

    // Drop CPU-side frame pointers while the GPU owns the submitted slot.
    m_commandList->endFrameReset();
    m_frameOpen = false;
}

//======================================================================================================================
void Metal4Device::waitIdle() {
    drainQueue(m_queue.get());
}

} // namespace lmx::rhi::metal4
