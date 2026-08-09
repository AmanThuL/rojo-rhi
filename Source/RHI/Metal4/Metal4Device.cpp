#include "RHI/Metal4/Metal4Device.h"

#include "RHI/CaptureSchema.h"
#include "RHI/Metal4/Metal4DevicePrivate.h"

#include <cstdlib>

namespace lmx::rhi::metal4 {
namespace {

using device_detail::describe;
using device_detail::fail;
using device_detail::toStdString;

constexpr NS::UInteger kMaxBufferBindCount = 8;
// The texture slot budget holds a pass-wide set and a per-draw material set at the same time, and
// eight no longer covers both. Buffers and samplers keep their own index spaces at eight; neither
// of those grew.
constexpr NS::UInteger kMaxTextureBindCount = 16;
constexpr NS::UInteger kMaxSamplerStateBindCount = 8;

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

    // Uniform rings share the frame rotation and device lifetime of the residency set.
    for (uint32_t i = 0; i < kFramesInFlight; ++i) {
        self->m_uniformRings[i] = NS::TransferPtr(
            self->m_device->newBuffer(kUniformRingBytes, MTL::ResourceStorageModeShared));
        if (!self->m_uniformRings[i]) {
            return fail(ErrorCode::ResourceCreationFailed,
                        "failed to create uniform ring " + std::to_string(i) + " of " +
                            std::to_string(kUniformRingBytes) + " bytes");
        }
        const std::string ringLabel = "lmx.device.uniformRing." + std::to_string(i);
        self->m_uniformRings[i]->setLabel(makeString(ringLabel).get());
        // Device-owned rings bypass resource wrappers, so register their capture identity here.
        debug::CaptureSchema::instance().registerBuffer(self->m_uniformRings[i].get(), ringLabel,
                                                        kUniformRingBytes);
        self->m_residency->addAllocation(self->m_uniformRings[i].get());
    }
    // A residency commit republishes the full set, so batch all ring additions into one commit.
    self->m_residency->commit();

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

    // Per-frame table and ring pointers are attached only while a frame is open.
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

    // Remove device-owned ring identities before their pointer values can be reused.
    for (const NS::SharedPtr<MTL::Buffer>& ring : m_uniformRings) {
        if (ring) {
            debug::CaptureSchema::instance().unregisterResource(ring.get());
        }
    }
}

} // namespace lmx::rhi::metal4

namespace lmx::rhi {

//======================================================================================================================
Result<std::unique_ptr<Device>> createDevice(const DeviceDesc& desc) {
    return metal4::Metal4Device::create(desc);
}

} // namespace lmx::rhi
