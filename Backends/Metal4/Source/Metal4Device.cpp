//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Device.cpp
/// @brief Creates and initializes the Metal 4 device and its persistent backend state.
//----------------------------------------------------------------------------------------------------------------------
#include "Metal4Device.h"

#include "Metal4DevicePrivate.h"
#include "RHI/CaptureSchema.h"

#include <cstdlib>

namespace lmx::rhi::metal4 {
namespace {

using device_detail::describe;
using device_detail::fail;
using device_detail::toStdString;

constexpr NS::UInteger kMaxBufferBindCount = CommandList::kMaxBufferBindings;
// The texture slot budget holds a pass-wide set and a per-draw material set at the same time, and
// eight no longer covers both. Buffers and samplers keep their own index spaces at eight; neither
// of those grew.
constexpr NS::UInteger kMaxTextureBindCount = CommandList::kMaxTextureBindings;
constexpr NS::UInteger kMaxSamplerStateBindCount = CommandList::kMaxSamplerBindings;

//======================================================================================================================
// Metal reads this switch when the device is created. Preserve an externally supplied value.
void requestMetalValidation() {
    ::setenv("MTL_DEBUG_LAYER", "1", 0);
}

} // namespace

//======================================================================================================================
Result<std::unique_ptr<Device>> Metal4Device::create(const DeviceDesc& desc) {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    if (desc.enableValidation) {
        requestMetalValidation();
    }

    auto self = std::unique_ptr<Metal4Device>(new Metal4Device());

    self->m_device = NS::TransferPtr(MTL::CreateSystemDefaultDevice());
    if (!self->m_device) {
        return fail(ErrorCode::DeviceUnsupported, "no Metal device available on this system");
    }
    self->m_deviceName = toStdString(self->m_device->name(), "<unnamed device>");

    if (!self->m_device->supportsFamily(MTL::GPUFamilyMetal4)) {
        return fail(ErrorCode::DeviceUnsupported,
                    "device '" + self->m_deviceName + "' does not support MTLGPUFamilyMetal4");
    }

    // Metal writes the error slot only on failure, so clear it before each call.
    NS::Error* error = nullptr;

    {
        auto queueDesc = NS::TransferPtr(MTL4::CommandQueueDescriptor::alloc()->init());
        queueDesc->setLabel(makeString("lmx.device.queue").get());
        error = nullptr;
        self->m_queue =
            NS::TransferPtr(self->m_device->newMTL4CommandQueue(queueDesc.get(), &error));
        if (!self->m_queue) {
            return fail(ErrorCode::DeviceUnsupported,
                        "failed to create MTL4 command queue: " + describe(error));
        }
    }

    {
        auto compilerDesc = NS::TransferPtr(MTL4::CompilerDescriptor::alloc()->init());
        compilerDesc->setLabel(makeString("lmx.device.compiler").get());
        error = nullptr;
        self->m_compiler = NS::TransferPtr(self->m_device->newCompiler(compilerDesc.get(), &error));
        if (!self->m_compiler) {
            return fail(ErrorCode::DeviceUnsupported,
                        "failed to create MTL4 compiler: " + describe(error));
        }
    }

    {
        auto residencyDesc = NS::TransferPtr(MTL::ResidencySetDescriptor::alloc()->init());
        residencyDesc->setLabel(makeString("lmx.device.residency").get());
        error = nullptr;
        self->m_residency =
            NS::TransferPtr(self->m_device->newResidencySet(residencyDesc.get(), &error));
        if (!self->m_residency) {
            return fail(ErrorCode::DeviceUnsupported,
                        "failed to create residency set: " + describe(error));
        }
        // Attach once; later residency commits republish allocations through the same set.
        self->m_residency->commit();
        self->m_queue->addResidencySet(self->m_residency.get());
    }

    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        auto allocatorDesc = NS::TransferPtr(MTL4::CommandAllocatorDescriptor::alloc()->init());
        allocatorDesc->setLabel(makeString("lmx.device.allocator." + std::to_string(i)).get());
        error = nullptr;
        self->m_allocators[i] =
            NS::TransferPtr(self->m_device->newCommandAllocator(allocatorDesc.get(), &error));
        if (!self->m_allocators[i]) {
            return fail(ErrorCode::DeviceUnsupported, "failed to create MTL4 command allocator " +
                                                          std::to_string(i) + ": " +
                                                          describe(error));
        }
    }

    self->m_frameEvent = NS::TransferPtr(self->m_device->newSharedEvent());
    if (!self->m_frameEvent) {
        return fail(ErrorCode::DeviceUnsupported, "failed to create frame-pacing shared event");
    }
    self->m_frameEvent->setLabel(makeString("lmx.device.frameEvent").get());
    self->m_frameEvent->setSignaledValue(0);
    self->m_frameNumber = 0;

    self->m_commandBuffer = NS::TransferPtr(self->m_device->newCommandBuffer());
    if (!self->m_commandBuffer) {
        return fail(ErrorCode::DeviceUnsupported, "failed to create MTL4 command buffer");
    }
    self->m_commandBuffer->setLabel(makeString("lmx.device.commandBuffer").get());

    // Argument tables rotate with the frame resources that reference their bindings.
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        auto tableDesc = NS::TransferPtr(MTL4::ArgumentTableDescriptor::alloc()->init());
        tableDesc->setMaxBufferBindCount(kMaxBufferBindCount);
        tableDesc->setMaxTextureBindCount(kMaxTextureBindCount);
        tableDesc->setMaxSamplerStateBindCount(kMaxSamplerStateBindCount);
        // Initialize unbound slots to null rather than stale GPU addresses.
        tableDesc->setInitializeBindings(true);
        tableDesc->setLabel(makeString("lmx.device.argumentTable." + std::to_string(i)).get());
        error = nullptr;
        self->m_argumentTables[i] =
            NS::TransferPtr(self->m_device->newArgumentTable(tableDesc.get(), &error));
        if (!self->m_argumentTables[i]) {
            return fail(ErrorCode::DeviceUnsupported, "failed to create MTL4 argument table " +
                                                          std::to_string(i) + ": " +
                                                          describe(error));
        }
    }

    // One normal page per slot up front, so a frame that stays inside its budget never allocates.
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        const Result<void> arena =
            self->m_frameArenas[i].create(self->m_device.get(), self->m_residency, i);
        if (!arena) {
            return std::unexpected(arena.error());
        }
    }

    // Pass timings are read back through the CPU-side resolve on the heap itself, so the heaps
    // need no residency registration and no staging buffer.
    self->m_timestampTicksPerSecond = self->m_device->queryTimestampFrequency();
    if (self->m_timestampTicksPerSecond == 0) {
        return fail(ErrorCode::DeviceUnsupported,
                    "device '" + self->m_deviceName +
                        "' reports a zero GPU timestamp frequency, so pass timings would have no "
                        "unit");
    }
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        auto heapDesc = NS::TransferPtr(MTL4::CounterHeapDescriptor::alloc()->init());
        heapDesc->setType(MTL4::CounterHeapTypeTimestamp);
        heapDesc->setCount(kTimestampsPerFrame);
        error = nullptr;
        Metal4FrameTimestamps& timestamps = self->m_frameTimestamps[i];
        timestamps.heap = NS::TransferPtr(self->m_device->newCounterHeap(heapDesc.get(), &error));
        if (!timestamps.heap) {
            return fail(ErrorCode::DeviceUnsupported, "failed to create timestamp counter heap " +
                                                          std::to_string(i) + ": " +
                                                          describe(error));
        }
        timestamps.heap->setLabel(
            makeString("lmx.device.timestampHeap." + std::to_string(i)).get());
        // A heap starts with undefined contents; the sentinel is what makes an unwritten entry
        // detectable rather than plausible.
        timestamps.heap->invalidateCounterRange(NS::Range::Make(0, kTimestampsPerFrame));
    }

    // Per-frame table and arena pointers are attached only while a frame is open.
    self->m_commandList.emplace(self->m_commandBuffer.get());

    return self;
}

//======================================================================================================================
Metal4Device::~Metal4Device() {
    NS::SharedPtr<NS::AutoreleasePool> pool = NS::TransferPtr(NS::AutoreleasePool::alloc()->init());

    // Drain before releasing allocators and events still referenced by submitted work.
    if (m_queue) {
        waitIdle();
    }

    // Remove the queue's ownership edge before releasing the residency set.
    if (m_queue && m_residency) {
        m_queue->removeResidencySet(m_residency.get());
    }

    for (Metal4FrameArena& arena : m_frameArenas) {
        arena.unregisterFromCapture();
    }

    // Everything below is what implicit member destruction would do anyway -- reverse declaration
    // order, which is reverse creation order -- performed by hand so that it happens *inside* this
    // pool. Implicit release runs after the body returns, with no pool on the thread, and Metal's
    // resource deallocs autorelease internally; see the destructor rule in Metal4Common.h. The
    // declaration order in Metal4Device.h stays the contract, and this sequence must keep matching
    // it, so that no ordering invariant depends on which of the two actually did the release.
    m_commandList.reset();
    // m_frameTimestamps is the one gap in the sequence, and deliberately so: Metal4FrameTimestamps
    // declares the counter heap, so it opens its own pool around releasing it and needs nothing
    // from here.
    for (Metal4FrameArena& arena : m_frameArenas) {
        arena.release();
    }
    for (NS::SharedPtr<MTL4::ArgumentTable>& table : m_argumentTables) {
        table.reset();
    }
    m_commandBuffer.reset();
    m_frameEvent.reset();
    for (NS::SharedPtr<MTL4::CommandAllocator>& allocator : m_allocators) {
        allocator.reset();
    }
    m_residency.reset();
    m_compiler.reset();
    m_queue.reset();
    m_device.reset();
}

//======================================================================================================================
FrameDataCounters Metal4Device::frameDataCounters() const {
    FrameDataCounters counters;
    counters.calls = m_frameDataTally.calls;
    counters.bytes = m_frameDataTally.bytes;
    counters.addressBinds = m_frameDataTally.addressBinds;
    for (uint32_t slot = 0; slot < kFramesInFlight; ++slot) {
        counters.pageCreations += m_frameArenas[slot].pageCreations();
        counters.slots[slot] = m_frameArenas[slot].counters();
    }
    return counters;
}

} // namespace lmx::rhi::metal4

namespace lmx::rhi {

//======================================================================================================================
Result<std::unique_ptr<Device>> createDevice(const DeviceDesc& desc) {
    return metal4::Metal4Device::create(desc);
}

} // namespace lmx::rhi
