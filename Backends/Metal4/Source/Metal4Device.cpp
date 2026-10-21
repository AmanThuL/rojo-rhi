//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Device.cpp
/// @brief Implements device creation and the public device entries for the Metal 4 backend.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Device.h"

#include <rojoRHI/RHI.h>

#include "Base/Assert.h"
#include "Base/Log.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/Sparse.h"
#include "Core/Swapchains.h"
#include "Metal4AddressMap.h"
#include "Metal4Buffers.h"
#include "Metal4Capabilities.h"
#include "Metal4Capture.h"
#include "Metal4Common.h"
#include "Metal4Fences.h"
#include "Metal4Formats.h"
#include "Metal4PipelineState.h"
#include "Metal4ShaderLibrary.h"
#include "Metal4Stager.h"
#include "Metal4Textures.h"
#include "Metal4Timestamps.h"
#include "Metal4Upscaler.h"
#include "Metal4Views.h"

#include <Metal/Metal.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace rojoRHI {
    namespace {
        /// The label of each caller-visible queue, indexed by `QueueType`.
        constexpr std::array<std::string_view, 3> kQueueLabels = { "rojorhi.queue.graphics", "rojorhi.queue.compute", "rojorhi.queue.copy" };

        //==============================================================================================================
        /// Waits until frame `frame` has completed on the graphics queue, which signals the frame event with the number of each
        /// frame it ends; counts the wait as one native host call.
        void waitFrame(void* context, uint64_t frame)
        {
            DeviceImpl& impl = *static_cast<DeviceImpl*>(context);
            metal4::waitUntilSignaled(impl.frameEvent, frame, "the frame event of device", impl.core.label());
            core::DeviceCounts::add(impl.core.counts().nativeHostCalls);
        }

        //==============================================================================================================
        /// Blocks until `queue`, labelled `label`, has finished the work submitted to it so far, through an event of its own,
        /// and then until Metal's completion work for those commits has let go of what it held, through a last empty commit
        /// that `metal4::commitAndWait` waits for: a list's allocator released afterwards is released for the last time here,
        /// not on Metal's notification thread, which holds no autorelease pool. Logs and aborts, in every build, when Metal
        /// makes no event, allocator or command buffer: the device's objects cannot be released while the queue may still
        /// use them. The caller holds an autorelease pool. Only `destroyDevice` calls it, after the caller's last submission:
        /// its empty commit is the one native commit on a caller queue that no `Queue::submit` makes, and it is not counted.
        void drainQueue(MTL::Device* device, MTL4::CommandQueue* queue, std::string_view label)
        {
            MTL::SharedEvent* drained = device->newSharedEvent();
            if (drained == nullptr)
            {
                ROJORHI_LOG_ERROR("Metal made no event to drain queue \"{}\" at device destruction", label);
                std::abort();
            }
            queue->signalEvent(drained, 1);
            metal4::waitUntilSignaled(drained, 1, "the drain event of queue", label);
            drained->release();
            MTL4::CommandAllocator* allocator = device->newCommandAllocator();
            MTL4::CommandBuffer* last = device->newCommandBuffer();
            if (allocator == nullptr || last == nullptr)
            {
                ROJORHI_LOG_ERROR("Metal made no command buffer to drain queue \"{}\" at device destruction", label);
                std::abort();
            }
            last->beginCommandBuffer(allocator);
            last->setLabel(metal4::makeString("rojorhi.drain"));
            last->endCommandBuffer();
            const std::array<const MTL4::CommandBuffer*, 1> buffers{ last };
            const std::string error = metal4::commitAndWait(queue, buffers, label);
            if (!error.empty())
            {
                ROJORHI_LOG_WARN("the drain commit on queue \"{}\" reported: {}", label, error);
            }
            last->release();
            allocator->release();
        }

        //==============================================================================================================
        /// Returns the command lists of a reused frame slot to its pool, each list's ended encoders released and its allocator
        /// reset, frees the swapchains destroyed in the slot's frames, readies the slot's timestamps, which the scope-time hook
        /// has read, and releases the events of destroyed fences every queue has passed. The frame wait before it has proven
        /// the slot's work complete, the compute and copy work included, which the frame's graphics work joins.
        void reuseSlot(void* context, uint32_t slot)
        {
            DeviceImpl& impl = *static_cast<DeviceImpl*>(context);
            {
                ListPool& pool = impl.listPools[slot];
                std::lock_guard lock(pool.mutex);
                // Freeing an encoder may autorelease; the hook runs on the caller's thread, whose pools RojoRHI does not fill.
                const metal4::AutoreleasePool autorelease;
                for (size_t i = 0; i < pool.used; ++i)
                {
                    pool.lists[i]->encoders.releaseEnded();
                    pool.lists[i]->state.allocator->reset();
                    core::DeviceCounts::add(impl.core.counts().nativeHostCalls);
                }
                pool.used = 0;
            }
            metal4::freeSlotSwapchains(impl, slot);
            impl.timestamps.reuse(slot, impl.core.counts());
            metal4::releasePassedFences(impl);
        }

        //==============================================================================================================
        /// Creates a Shared buffer of `entries` `MTL::ResourceID`s labelled `label`, or returns null. The caller holds an
        /// autorelease pool.
        MTL::Buffer* newIdTable(MTL::Device* device, uint64_t entries, std::string_view label)
        {
            MTL::Buffer* table = device->newBuffer(entries * sizeof(MTL::ResourceID), MTL::ResourceStorageModeShared | MTL::ResourceCPUCacheModeDefaultCache);
            if (table != nullptr)
            {
                table->setLabel(metal4::makeString(label));
            }
            return table;
        }

        //==============================================================================================================
        /// Releases the native objects of `impl` that exist, in reverse order of creation, and clears each pointer. The caller
        /// holds an autorelease pool, and no queue holds work that uses them.
        void releaseNative(DeviceImpl& impl)
        {
            metal4::releaseRetiringFences(impl);
            if (impl.samplerTable != nullptr)
            {
                impl.residency.remove(impl.samplerTable);
                impl.samplerTable->release();
                impl.samplerTable = nullptr;
            }
            if (impl.viewTable != nullptr)
            {
                impl.residency.remove(impl.viewTable);
                impl.viewTable->release();
                impl.viewTable = nullptr;
            }
            if (impl.frameEvent != nullptr)
            {
                impl.frameEvent->release();
                impl.frameEvent = nullptr;
            }
            if (impl.residencyAttached)
            {
                impl.residency.detach(impl.stager.queue());
                for (QueueImpl& queue : impl.queues)
                {
                    impl.residency.detach(queue.native);
                }
                impl.residencyAttached = false;
            }
            impl.stager.release();
            impl.residency.release();
            impl.depthStencils.release(impl.core.counts());
            impl.kernels.release();
            if (impl.compiler != nullptr)
            {
                impl.compiler->release();
                impl.compiler = nullptr;
            }
            impl.pipelineCache.release();
            for (size_t i = impl.queues.size(); i-- > 0;)
            {
                QueueImpl& queue = impl.queues[i];
                if (queue.carry != nullptr)
                {
                    queue.carry->release();
                    queue.carry = nullptr;
                }
                if (queue.progress != nullptr)
                {
                    queue.progress->release();
                    queue.progress = nullptr;
                }
                if (queue.native != nullptr)
                {
                    queue.native->release();
                    queue.native = nullptr;
                }
            }
            if (impl.device != nullptr)
            {
                impl.device->release();
                impl.device = nullptr;
            }
        }

        //==============================================================================================================
        /// Creates the native objects of `impl` on its device, in the order `releaseNative` reverses. The caller holds an
        /// autorelease pool and releases what exists when this fails. As on the Null backend, which counts nothing at device
        /// creation or destruction, the device's own objects and the calls that create, drain and release them are not counted:
        /// the counters measure frames and the objects a caller creates.
        Result<void> createNative(DeviceImpl& impl)
        {
            NS::Error* error = nullptr;
            for (size_t i = 0; i < impl.queues.size(); ++i)
            {
                MTL4::CommandQueueDescriptor* descriptor = MTL4::CommandQueueDescriptor::alloc()->init();
                descriptor->setLabel(metal4::makeString(kQueueLabels[i]));
                error = nullptr;
                impl.queues[i].native = impl.device->newMTL4CommandQueue(descriptor, &error);
                descriptor->release();
                if (impl.queues[i].native == nullptr)
                {
                    return std::unexpected(
                        Error{ ErrorCategory::DriverFailure, std::format("the queue {} was not created: {}", kQueueLabels[i], metal4::describe(error)) }
                    );
                }
                impl.queues[i].progress = impl.device->newSharedEvent();
                if (impl.queues[i].progress == nullptr)
                {
                    return std::unexpected(Error{ ErrorCategory::OutOfMemory, std::format("the progress event of queue {} was not created", kQueueLabels[i]) });
                }
                impl.queues[i].progress->setLabel(metal4::makeString(std::format("{}.progress", kQueueLabels[i])));
                if (static_cast<QueueType>(i) == QueueType::Graphics)
                {
                    impl.queues[i].carry = impl.device->newFence();
                    if (impl.queues[i].carry == nullptr)
                    {
                        return std::unexpected(
                            Error{ ErrorCategory::OutOfMemory, std::format("the carry fence of queue {} was not created", kQueueLabels[i]) }
                        );
                    }
                    impl.queues[i].carry->setLabel(metal4::makeString(std::format("{}.carry", kQueueLabels[i])));
                }
            }
            if (Result<void> created = impl.pipelineCache.create(impl.device); !created)
            {
                return created;
            }
            MTL4::CompilerDescriptor* compilerDescriptor = MTL4::CompilerDescriptor::alloc()->init();
            compilerDescriptor->setLabel(metal4::makeString("rojorhi.compiler"));
            compilerDescriptor->setPipelineDataSetSerializer(impl.pipelineCache.serializer());
            error = nullptr;
            impl.compiler = impl.device->newCompiler(compilerDescriptor, &error);
            compilerDescriptor->release();
            if (impl.compiler == nullptr)
            {
                return std::unexpected(Error{ ErrorCategory::DriverFailure, std::format("the compiler was not created: {}", metal4::describe(error)) });
            }
            if (Result<void> created = impl.kernels.create(impl.device, impl.compiler); !created)
            {
                return created;
            }
            if (Result<void> created = impl.depthStencils.create(impl.device, impl.core.counts()); !created)
            {
                return created;
            }
            if (Result<void> created = impl.residency.create(impl.device); !created)
            {
                return created;
            }
            if (Result<void> created = impl.stager.create(impl.device); !created)
            {
                return created;
            }
            for (QueueImpl& queue : impl.queues)
            {
                impl.residency.attach(queue.native);
            }
            impl.residency.attach(impl.stager.queue());
            impl.residencyAttached = true;
            impl.frameEvent = impl.device->newSharedEvent();
            if (impl.frameEvent == nullptr)
            {
                return std::unexpected(Error{ ErrorCategory::OutOfMemory, "the frame event was not created" });
            }
            impl.frameEvent->setSignaledValue(0);
            impl.frameEvent->setLabel(metal4::makeString("rojorhi.frame"));
            const Capabilities& capabilities = impl.core.capabilities();
            impl.viewTable = newIdTable(impl.device, capabilities.maxViews, "rojorhi.viewTable");
            if (impl.viewTable == nullptr)
            {
                return std::unexpected(
                    Error{ ErrorCategory::OutOfMemory, std::format("no device memory for the view table of {} entries", capabilities.maxViews) }
                );
            }
            impl.residency.add(impl.viewTable);
            impl.samplerTable = newIdTable(impl.device, capabilities.maxSamplers, "rojorhi.samplerTable");
            if (impl.samplerTable == nullptr)
            {
                return std::unexpected(
                    Error{ ErrorCategory::OutOfMemory, std::format("no device memory for the sampler table of {} entries", capabilities.maxSamplers) }
                );
            }
            impl.residency.add(impl.samplerTable);
            // The set stays dirty: the first commit that needs it, a submission or a staged upload, makes the tables resident.
            return {};
        }

        //==============================================================================================================
        /// Reports what `impl`'s device supports: its names, limits and format table, and the features this backend does not
        /// provide yet as absent.
        void reportCapabilities(DeviceImpl& impl)
        {
            impl.adapterName = metal4::toStdString(impl.device->name());
            impl.floorName = metal4::floorName(impl.device);
            Capabilities& capabilities = impl.core.capabilities();
            capabilities.adapterName = impl.adapterName;
            capabilities.floorName = impl.floorName;
            capabilities.limits = metal4::metalLimits(impl.device);
            capabilities.maxViews = capabilities.limits.maxViews;
            capabilities.maxSamplers = capabilities.limits.maxSamplers;
            capabilities.uploadInDeviceMemory = true; // Unified memory.
            // Sparse resources are placement sparse resources with 64 KiB pages.
            capabilities.placementSparse = impl.device->supportsPlacementSparse();
            capabilities.meshTasksIndirect = impl.device->supportsFamily(MTL::GPUFamilyApple9);
            capabilities.conservativeRaster = false; // Metal 4 has no conservative rasterization.
            capabilities.upscalers = metal4::metalUpscalers(impl.device);
            impl.core.setFormatTable(metal4::metalFormatTable(impl.device));
        }
    } // namespace

    //==================================================================================================================
    DeviceImpl& DeviceImpl::of(Device device)
    {
        core::checkObject(device.m_impl, "Device");
        return *device.m_impl;
    }

    //==================================================================================================================
    Device DeviceImpl::wrap(DeviceImpl* impl)
    {
        Device device;
        device.m_impl = impl;
        return device;
    }

    namespace core {
        //==============================================================================================================
        DeviceCore& coreOf(Device device)
        {
            return DeviceImpl::of(device).core;
        }

        //==============================================================================================================
        SwapchainCore& coreOf(Swapchain swapchain)
        {
            return SwapchainImpl::of(swapchain).core;
        }
    } // namespace core

    //==================================================================================================================
    Result<Device> createDevice(const DeviceDesc& desc)
    {
        if (Result<void> valid = core::validateDeviceDesc(desc); !valid)
        {
            return std::unexpected(std::move(valid.error()));
        }
        const metal4::AutoreleasePool pool;
        // DeviceDesc::enableCapture only makes encoders carry labels (encoderLabels, below).
        MTL::Device* device = MTL::CreateSystemDefaultDevice();
        if (device == nullptr)
        {
            return std::unexpected(Error{ ErrorCategory::Unsupported, "no Metal device" });
        }
        if (Result<void> floor = metal4::checkFloor(device); !floor)
        {
            device->release();
            return std::unexpected(std::move(floor.error()));
        }
        auto impl = std::make_unique<DeviceImpl>(desc);
        impl->device = device;
        impl->commandPools = metal4::commandsMayAutorelease(device);
        impl->encoderLabels = desc.validation || desc.enableCapture || impl->commandPools;
        impl->timestamps.create(device);
        reportCapabilities(*impl);
        impl->core.setSurfaceCheck(metal4::isMetalLayer);
        if (Result<void> created = createNative(*impl); !created)
        {
            releaseNative(*impl);
            return std::unexpected(std::move(created.error()));
        }
        // The archive is loaded once the compiler exists and before any pipeline is compiled; a refused blob is a warning.
        impl->pipelineCache.load(impl->device, desc.pipelineCache);
        core::RetireHooks hooks;
        hooks.context = impl.get();
        hooks.frameSlot = reuseSlot;
        hooks.waitFrame = waitFrame;
        hooks.buffer = metal4::retireBuffer;
        hooks.heap = metal4::retireHeap;
        hooks.texture = metal4::retireTexture;
        hooks.view = metal4::retireView;
        hooks.sampler = metal4::retireSampler;
        hooks.fence = metal4::retireFence;
        hooks.pipeline = metal4::retirePipeline;
        hooks.shaderLibrary = metal4::retireShaderLibrary;
        hooks.scopeTimes = metal4::scopeTimes;
        hooks.upscaler = metal4::retireUpscaler;
        impl->core.setRetireHooks(hooks);
        impl->core.setArenaSource(metal4::createArenaPage);
        metal4::setMaxAddrOffset(impl->core.capabilities().limits.maxAddrOffset);
        impl->core.setCaptureAvailable(metal4::startCapture);
        return DeviceImpl::wrap(impl.release());
    }

    //==================================================================================================================
    void destroyDevice(Device device)
    {
        DeviceImpl& impl = DeviceImpl::of(device);
        {
            const metal4::AutoreleasePool pool;
            // Every queue finishes its work before any object it may use is released, so the hooks of beginDestroy run on idle
            // queues; VU-rojo-device-0002, which beginDestroy checks, therefore fires after the drain. The drain is not counted.
            for (const QueueImpl& queue : impl.queues)
            {
                drainQueue(impl.device, queue.native, kQueueLabels[static_cast<size_t>(queue.type)]);
            }
            drainQueue(impl.device, impl.stager.queue(), "rojorhi.upload");
            impl.core.beginDestroy();
            // The lists' objects go before the device's, which they were created on.
            for (ListPool& listPool : impl.listPools)
            {
                for (const std::unique_ptr<CommandListImpl>& list : listPool.lists)
                {
                    list->encoders.release();
                    list->draw.release(impl.residency);
                    list->state.release();
                }
            }
            metal4::releaseSwapchains(impl);
            // The heaps go once no queue can still write a timestamp into them.
            impl.timestamps.release();
            releaseNative(impl);
        }
        delete &impl;
    }

    //==================================================================================================================
    const Capabilities& Device::capabilities() const
    {
        return DeviceImpl::of(*this).core.capabilities();
    }

    //==================================================================================================================
    FormatCapabilities Device::formatCapabilities(Format format) const
    {
        return DeviceImpl::of(*this).core.formatCapabilities(format);
    }

    //==================================================================================================================
    void Device::beginFrame()
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        // The core waits for the frame whose slot it reuses through the waitFrame hook, after its own checks.
        impl.core.beginFrame();
        // Once a counted draw's kernels have run, the lists of this frame on carry the dispatch stage of a barrier they owe at
        // their end through the carry fence, which those kernels do not wait on; every list of the earlier frames, which used
        // no fence, was submitted before them.
        QueueImpl& graphics = impl.queues[static_cast<size_t>(QueueType::Graphics)];
        if (!graphics.carryFenced.load(std::memory_order_relaxed) && graphics.kernelsRan.load(std::memory_order_relaxed))
        {
            graphics.carryFenced.store(true, std::memory_order_release);
        }
    }

    //==================================================================================================================
    void Device::endFrame()
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        impl.core.endFrame();
        impl.queues[static_cast<size_t>(QueueType::Graphics)].native->signalEvent(impl.frameEvent, impl.core.frameNumber());
        core::DeviceCounts::add(impl.core.counts().nativeSubmitCalls);
    }

    //==================================================================================================================
    Queue Device::queue(QueueType type)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        const size_t index = static_cast<size_t>(type);
        ROJORHI_INTERNAL_ASSERT(index < impl.queues.size(), "a QueueType outside the enumerators has no queue");
        return QueueImpl::wrap(&impl.queues[index]);
    }

    //==================================================================================================================
    Counters Device::counters() const
    {
        return DeviceImpl::of(*this).core.counters();
    }

    //==================================================================================================================
    std::span<const ScopeTiming> Device::scopeTimings()
    {
        return DeviceImpl::of(*this).core.scopeTimings();
    }

    //==================================================================================================================
    TileShape Device::tileShape(Texture texture)
    {
        return core::tileShape(DeviceImpl::of(*this).core, texture);
    }

    //==================================================================================================================
    TileShape Device::tileShape(Buffer buffer)
    {
        return core::tileShape(DeviceImpl::of(*this).core, buffer);
    }
} // namespace rojoRHI
