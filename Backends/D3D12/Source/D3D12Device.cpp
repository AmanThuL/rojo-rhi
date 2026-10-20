//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Device.cpp
/// @brief Implements D3D12 device creation and teardown, the handle seams and the device entries the core serves whole.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Device.h"

#include <rojoRHI/RHI.h>

#include "Base/Assert.h"
#include "Core/Captures.h"
#include "Core/DeviceCore.h"
#include "Core/Sparse.h"
#include "D3D12Buffers.h"
#include "D3D12Capabilities.h"
#include "D3D12Common.h"
#include "D3D12Fences.h"
#include "D3D12Loader.h"
#include "D3D12ShaderLibrary.h"
#include "D3D12Textures.h"
#include "D3D12Views.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace rojoRHI {
    namespace {
        //==============================================================================================================
        /// Returns true when `surface` is a window, the native surface of the D3D12 backend (VU-rojo-present-0002).
        bool isWindow(const void* surface)
        {
            return IsWindow(static_cast<HWND>(const_cast<void*>(surface))) != FALSE;
        }

        //==============================================================================================================
        /// Creates the device-wide native state of `impl` in declaration order, the order `teardown` destroys it in reverse.
        Result<void> createState(DeviceImpl& impl)
        {
            impl.messages.attach(impl.device(), impl.loaded.layer);
            if (Result<void> created = d3d12::createQueues(impl); !created)
            {
                return created;
            }
            if (Result<void> created = impl.frames.create(impl); !created)
            {
                return created;
            }
            if (Result<void> created = impl.descriptors.create(impl); !created)
            {
                return created;
            }
            impl.directory.create(impl.descriptors.viewCapacity());
            if (Result<void> reported = d3d12::reportCapabilities(impl); !reported)
            {
                return reported;
            }
            if (Result<void> created = impl.roots.create(impl); !created)
            {
                return created;
            }
            return impl.stager.create(impl);
        }

        //==============================================================================================================
        /// Tears down `impl`, made by `createDevice` whether or not all of its state was created: the queues finish their work,
        /// the core passes every remaining record to the retire hooks, the kept fences go, and then every native object is
        /// released in reverse order of creation, the device last. A Debug build checks that no reference to the device
        /// outlives it.
        void teardown(DeviceImpl* impl)
        {
            d3d12::idleQueues(*impl);
            impl->stager.idle();
            impl->core.beginDestroy();
            impl->frames.releaseAll();
            // The factory that made the device outlives it, and the device's reference is the last one released.
            d3d12::Com<ID3D12DeviceFactory> factory = impl->loaded.factory;
            ID3D12Device12* device = impl->loaded.device.detach();
            delete impl;
            [[maybe_unused]] const ULONG remaining = device->Release();
            ROJORHI_INTERNAL_ASSERT(remaining == 0, "a reference to the D3D12 device outlived destroyDevice");
            factory.reset();
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

    //==================================================================================================================
    QueueImpl& QueueImpl::of(Queue queue)
    {
        core::checkObject(queue.m_impl, "Queue");
        return *queue.m_impl;
    }

    //==================================================================================================================
    Queue QueueImpl::wrap(QueueImpl* impl)
    {
        Queue queue;
        queue.m_impl = impl;
        return queue;
    }

    //==================================================================================================================
    CommandListImpl& CommandListImpl::of(CommandList list)
    {
        core::checkObject(list.m_impl, "CommandList");
        return *list.m_impl;
    }

    //==================================================================================================================
    CommandList CommandListImpl::wrap(CommandListImpl* impl)
    {
        CommandList list;
        list.m_impl = impl;
        return list;
    }

    //==================================================================================================================
    SwapchainImpl& SwapchainImpl::of(Swapchain swapchain)
    {
        core::checkObject(swapchain.m_impl, "Swapchain");
        return *swapchain.m_impl;
    }

    //==================================================================================================================
    Swapchain SwapchainImpl::wrap(SwapchainImpl* impl)
    {
        Swapchain swapchain;
        swapchain.m_impl = impl;
        return swapchain;
    }

    namespace core {
        //==============================================================================================================
        DeviceCore& coreOf(Device device)
        {
            return DeviceImpl::of(device).core;
        }
    } // namespace core

    //==================================================================================================================
    Result<Device> createDevice(const DeviceDesc& desc)
    {
        if (Result<void> valid = core::validateDeviceDesc(desc); !valid)
        {
            return std::unexpected(std::move(valid.error()));
        }
        // DeviceDesc::enableCapture and DeviceDesc::pipelineCache are not read: the backend has no capture tool and no
        // pipeline library yet.
        Result<d3d12::Loaded> loaded = d3d12::loadDevice();
        if (!loaded)
        {
            return std::unexpected(std::move(loaded.error()));
        }
        auto impl = std::make_unique<DeviceImpl>(desc, std::move(*loaded));
        d3d12::setName(impl->device(), impl->core.label());
        core::RetireHooks hooks;
        hooks.context = impl.get();
        hooks.buffer = d3d12::retireBuffer;
        hooks.heap = d3d12::retireHeap;
        hooks.texture = d3d12::retireTexture;
        hooks.view = d3d12::retireView;
        hooks.fence = d3d12::retireFence;
        hooks.shaderLibrary = d3d12::retireShaderLibrary;
        hooks.frameSlot = d3d12::reuseSlot;
        hooks.waitFrame = d3d12::waitFrame;
        impl->core.setRetireHooks(hooks);
        if (Result<void> created = createState(*impl); !created)
        {
            teardown(impl.release());
            return std::unexpected(std::move(created.error()));
        }
        impl->core.setSurfaceCheck(isWindow);
        impl->core.setArenaSource(d3d12::createArenaPage);
        return DeviceImpl::wrap(impl.release());
    }

    //==================================================================================================================
    void destroyDevice(Device device)
    {
        teardown(&DeviceImpl::of(device));
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
    Result<void> Device::beginCapture(std::string_view path)
    {
        // No capture hook is registered, so the core refuses with Unsupported after its checks.
        return core::beginCapture(DeviceImpl::of(*this).core, path);
    }

    //==================================================================================================================
    void Device::endCapture()
    {
        core::endCapture(DeviceImpl::of(*this).core);
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

    //==================================================================================================================
    void Device::destroy(Pipeline pipeline)
    {
        DeviceImpl::of(*this).core.destroy(pipeline);
    }

    //==================================================================================================================
    void Device::destroy(ShaderLibrary library)
    {
        DeviceImpl::of(*this).core.destroy(library);
    }

    //==================================================================================================================
    void Device::destroy(Upscaler upscaler)
    {
        DeviceImpl::of(*this).core.destroy(upscaler);
    }

    //==================================================================================================================
    Result<Upscaler> Device::createUpscaler(const UpscalerDesc&)
    {
        return d3d12::unsupported("Device::createUpscaler");
    }
} // namespace rojoRHI
