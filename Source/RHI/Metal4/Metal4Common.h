#pragma once
// Shared plumbing for the Metal 4 backend: the metal-cpp umbrella includes plus the
// handful of helpers every backend file needs. Private to the RHI target -- metal-cpp
// types never appear in RHI.h.
#include "Core/Assert.h"
#include "RHI/RHI.h"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <QuartzCore/QuartzCore.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace lmx::rhi::metal4 {

// RHI descs carry std::string_view, which is NOT guaranteed NUL-terminated, so the
// text is copied into a std::string before it reaches NS::String's char* initializer.
// Returns an owning (+1) reference; the SharedPtr releases it at end of scope, by
// which point whatever consumed it (setLabel, ...) has taken its own reference.
inline NS::SharedPtr<NS::String> makeString(std::string_view text) {
    const std::string owned(text);
    return NS::TransferPtr(NS::String::alloc()->init(owned.c_str(), NS::UTF8StringEncoding));
}

inline MTL::PixelFormat toMTL(Format format) {
    switch (format) {
    case Format::BGRA8Unorm:
        return MTL::PixelFormatBGRA8Unorm;
    case Format::RGBA8Unorm:
        return MTL::PixelFormatRGBA8Unorm;
    case Format::RGBA8Unorm_sRGB:
        return MTL::PixelFormatRGBA8Unorm_sRGB;
    // Metal names the DXT1 formats BC1_RGBA rather than BC1_RGB: the one-bit-alpha and
    // opaque encodings share a single pixel format, and which one a block uses is decided
    // per block by the ordering of its two endpoint colours.
    case Format::BC1Unorm:
        return MTL::PixelFormatBC1_RGBA;
    case Format::BC1Unorm_sRGB:
        return MTL::PixelFormatBC1_RGBA_sRGB;
    case Format::D32Float:
        return MTL::PixelFormatDepth32Float;
    case Format::Unknown:
        break;
    }
    return MTL::PixelFormatInvalid;
}

// The conservative Metal constant-buffer offset-alignment bound; Apple GPUs accept less, but 256
// is correct everywhere. A backend constant rather than a Device one -- Metal4Device.h's
// kUniformRingBytes is the only thing that budgets against it, and Metal4CommandList::setUniforms
// is the only thing that rounds against it (Core/Align.h's alignUp), so it belongs with the rest
// of the backend's shared plumbing rather than pinned to the device that happens to consume it.
inline constexpr uint64_t kUniformOffsetAlignment = 256;

// Generous: any wait longer than this means the GPU is wedged, not busy.
inline constexpr uint64_t kGpuTimeoutMs = 10'000;

// Blocks until every command buffer already committed to `queue` has completed.
//
// Shared by Device::waitIdle() and by ~Metal4Swapchain, which must drain before it detaches the
// layer's residency set from the queue. It deliberately takes only the queue: MTL4::CommandQueue
// exposes device(), so no back-pointer to Metal4Device is needed and a swapchain can drain
// safely without knowing anything about the object that created it.
//
// A throwaway event rather than the device's frame-pacing event: that event's value sequence is
// owned by beginFrame/endFrame, and signalling an out-of-band value on it would corrupt pacing.
// The queue signals in submission order, so once this fires every command buffer committed
// before it has completed.
inline void drainQueue(MTL4::CommandQueue* queue) {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    LMX_ASSERT(queue != nullptr, "drainQueue: queue must not be null");
    MTL::Device* device = queue->device();
    LMX_ASSERT(device != nullptr, "drainQueue: command queue has no device");

    NS::SharedPtr<MTL::SharedEvent> done = NS::TransferPtr(device->newSharedEvent());
    LMX_ASSERT(done, "drainQueue: failed to create shared event");
    done->setLabel(makeString("lmx.queue.drain").get());
    done->setSignaledValue(0);

    queue->signalEvent(done.get(), 1);
    const bool signaled = done->waitUntilSignaledValue(1, kGpuTimeoutMs);
    LMX_ASSERT(signaled, "drainQueue: GPU did not complete within the timeout");
}

} // namespace lmx::rhi::metal4
