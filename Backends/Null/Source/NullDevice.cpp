//----------------------------------------------------------------------------------------------------------------------
/// @file NullDevice.cpp
/// @brief Implements the public device entries for the Null backend by calling the shared core.
//----------------------------------------------------------------------------------------------------------------------

#include "NullDevice.h"

#include <rojoRHI/Device.h>

#include "Base/Assert.h"
#include "Core/Buffers.h"
#include "Core/DeviceCore.h"
#include "Core/Fences.h"
#include "Core/Formats.h"
#include "Core/Heaps.h"
#include "Core/Ranges.h"
#include "Core/Samplers.h"
#include "Core/Textures.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>

namespace rojoRHI {
    namespace {
        constexpr std::array<UpscalerKind, 4> kUpscalers = { UpscalerKind::MetalFX, UpscalerKind::DLSS, UpscalerKind::FSR, UpscalerKind::XeSS };

        //==============================================================================================================
        /// Each field takes the Floor column of the limits table in the reference where that column gives a number, and
        /// otherwise the smaller of the Metal 4 and D3D12 values, so a program that fits the Null device fits both planned
        /// backends. The comment names the source of each value.
        Limits nullLimits()
        {
            Limits limits;
            limits.maxViews = 1'000'000;      // Floor.
            limits.maxSamplers = 996;         // Floor.
            limits.maxRootBlockSize = 65'536; // Floor.
            limits.rootAlignment = 256;       // Floor.
            limits.frameDataAlignment = 256;  // Floor.
            limits.maxBufferSize =
                1ull << 32; // Unverified: the Floor column names no number ("device"); 4 GiB, an arbitrary value above the 2^32 - 1 Addr reach.
            limits.maxAddrOffset = kNullMaxAddrOffset; // Floor, 2^32 - 1.
            limits.maxTextureDimension1D = 16'384;     // Floor.
            limits.maxTextureDimension2D = 16'384;     // Floor.
            limits.maxTextureDimension3D = 2'048;      // Floor.
            limits.maxTextureDimensionCube = 16'384;   // Floor.
            limits.maxTextureLayers = 2'048;           // Floor.
            limits.maxColorAttachments = 8;            // Floor.
            limits.maxColorBytesPerSample = 64;        // Floor.
            limits.maxSamplerAnisotropy = 16;          // Floor.
            limits.maxDrawIndirectCount = 65'535;      // Floor.
            limits.maxDispatchGroups = 65'535;         // Floor.
            limits.maxThreadsPerGroup = 1'024;         // Floor.
            limits.maxGroupSize[0] = 1'024;            // Floor.
            limits.maxGroupSize[1] = 1'024;            // Floor.
            limits.maxGroupSize[2] = 64;               // Floor.
            limits.maxGroupSharedMemory = 32'768;      // Floor.
            limits.maxMeshTaskGroups = 65'535;         // Floor.
            limits.maxMeshTaskGroupsTotal = 4'194'304; // Floor.
            return limits;
        }

        //==============================================================================================================
        /// The Null device's placement answer for a buffer: the size rounded up to 256 bytes, at an alignment of 256 bytes. The
        /// values are the Null backend's own choice, a round number that is neither Metal's nor D3D12's, so a caller that
        /// hard-codes either finds out here.
        PlacementRequirements nullBufferRequirements(const BufferDesc& desc)
        {
            constexpr uint64_t kAlignment = 256;
            return PlacementRequirements{ (desc.size + kAlignment - 1) / kAlignment * kAlignment, kAlignment };
        }

        //==============================================================================================================
        /// The Null device's placement answer for a texture: the packed bytes of every mip, layer and sample rounded up to 1024
        /// bytes, at an alignment of 1024 bytes. Like the buffer answer it is the Null backend's own round number, neither
        /// Metal's nor D3D12's. Written to be safe for a descriptor the core has not checked yet.
        PlacementRequirements nullTextureRequirements(const TextureDesc& desc)
        {
            constexpr uint64_t kAlignment = 1024;
            core::FormatLayout layout = core::formatLayout(desc.format);
            if (desc.format == Format::D32FloatS8)
            {
                layout.blockBytes = 8; // The Null device's own size for a format with no packed texel size.
            }
            uint64_t bytes = 0;
            for (uint32_t mip = 0; mip < std::min(desc.mips, 32u); ++mip)
            {
                const uint64_t width = std::max(desc.extent.width >> mip, 1u);
                const uint64_t height = std::max(desc.extent.height >> mip, 1u);
                const uint64_t depth = std::max(desc.extent.depth >> mip, 1u);
                bytes +=
                    (width + layout.blockWidth - 1) / layout.blockWidth * ((height + layout.blockHeight - 1) / layout.blockHeight) * depth * layout.blockBytes;
            }
            bytes *= uint64_t{ desc.layers } * desc.samples;
            return PlacementRequirements{ (std::max(bytes, uint64_t{ 1 }) + kAlignment - 1) & ~(kAlignment - 1), kAlignment };
        }

        //==============================================================================================================
        /// Frees the host memory of a retiring buffer; a Device buffer holds none.
        void retireBuffer(void*, core::BufferRecord& record)
        {
            std::free(record.native);
            record.native = nullptr;
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
    } // namespace core

    //==================================================================================================================
    Result<Device> createDevice(const DeviceDesc& desc)
    {
        if (Result<void> valid = core::validateDeviceDesc(desc); !valid)
        {
            return std::unexpected(std::move(valid.error()));
        }
        auto impl = std::make_unique<DeviceImpl>(desc);
        Capabilities& capabilities = impl->core.capabilities();
        capabilities.adapterName = "Null";
        capabilities.floorName = "Null";
        capabilities.limits = nullLimits();
        capabilities.maxViews = capabilities.limits.maxViews;
        capabilities.maxSamplers = capabilities.limits.maxSamplers;
        capabilities.uploadInDeviceMemory = true;
        capabilities.placementSparse = true;
        capabilities.meshTasksIndirect = true;
        capabilities.conservativeRaster = true;
        capabilities.upscalers = kUpscalers;
        impl->core.setFormatTable(core::formatCeilings());
        core::RetireHooks hooks;
        hooks.context = impl.get();
        hooks.buffer = retireBuffer;
        impl->core.setRetireHooks(hooks);
        return DeviceImpl::wrap(impl.release());
    }

    //==================================================================================================================
    void destroyDevice(Device device)
    {
        DeviceImpl& impl = DeviceImpl::of(device);
        impl.core.beginDestroy();
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
        DeviceImpl::of(*this).core.beginFrame();
    }

    //==================================================================================================================
    void Device::endFrame()
    {
        DeviceImpl::of(*this).core.endFrame();
    }

    //==================================================================================================================
    Result<Buffer> Device::createBuffer(const BufferDesc& desc, std::span<const std::byte> initial, Placement placement)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        const PlacementRequirements requirements = placement.heap.bits != 0 ? nullBufferRequirements(desc) : PlacementRequirements{};
        core::BufferRecord record = core::prepareBuffer(impl.core, desc, initial, placement, requirements);
        // Only Upload and Readback buffers get host memory; a Gpu buffer keeps its record and drops its initial data.
        if (desc.memory != Memory::Gpu)
        {
            auto* host = static_cast<std::byte*>(std::calloc(desc.size, 1));
            if (host == nullptr)
            {
                return std::unexpected(
                    Error{ ErrorCategory::OutOfMemory, std::format("no host memory for {} bytes of buffer \"{}\"", desc.size, record.label) }
                );
            }
            if (!initial.empty())
            {
                std::memcpy(host, initial.data(), initial.size());
            }
            record.native = host;
        }
        return core::addBuffer(impl.core, std::move(record));
    }

    //==================================================================================================================
    Result<Heap> Device::createHeap(const HeapDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        // A Null heap owns no memory beyond its record.
        return impl.core.pool<Heap>().allocate(core::prepareHeap(desc));
    }

    //==================================================================================================================
    PlacementRequirements Device::placementRequirements(const BufferDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::checkBufferRequirementsQuery(impl.core, desc);
        return nullBufferRequirements(desc);
    }

    //==================================================================================================================
    PlacementRequirements Device::placementRequirements(const TextureDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::checkTextureRequirementsQuery(impl.core, desc);
        return nullTextureRequirements(desc);
    }

    //==================================================================================================================
    Result<Texture> Device::createTexture(const TextureDesc& desc, std::span<const TextureData> initial, Placement placement)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        const PlacementRequirements requirements = placement.heap.bits != 0 ? nullTextureRequirements(desc) : PlacementRequirements{};
        // A Null texture keeps its record and drops its texels, initial data included.
        return core::addTexture(impl.core, core::prepareTexture(impl.core, desc, initial, placement, requirements));
    }

    //==================================================================================================================
    Result<ViewIndex> Device::createView(Texture texture, const TextureViewDesc& desc)
    {
        return core::createView(DeviceImpl::of(*this).core, texture, desc);
    }

    //==================================================================================================================
    Result<SamplerIndex> Device::createSampler(const SamplerDesc& desc)
    {
        return core::createSampler(DeviceImpl::of(*this).core, desc);
    }

    //==================================================================================================================
    Result<IndexRange> Device::reserveViews(uint32_t count)
    {
        return core::reserveViews(DeviceImpl::of(*this).core, count);
    }

    //==================================================================================================================
    void Device::writeView(IndexRange range, uint32_t index, Texture texture, const TextureViewDesc& desc)
    {
        core::writeView(DeviceImpl::of(*this).core, range, index, texture, desc);
    }

    //==================================================================================================================
    Result<IndexRange> Device::reserveSamplers(uint32_t count)
    {
        return core::reserveSamplers(DeviceImpl::of(*this).core, count);
    }

    //==================================================================================================================
    void Device::writeSampler(IndexRange range, uint32_t index, const SamplerDesc& desc)
    {
        core::checkSamplerWrite(DeviceImpl::of(*this).core, range, index, desc);
    }

    //==================================================================================================================
    void Device::release(IndexRange range)
    {
        core::releaseRange(DeviceImpl::of(*this).core, range);
    }

    //==================================================================================================================
    Addr Device::address(Buffer buffer, uint64_t offset)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::checkBufferAddress(core::checkedBuffer(impl.core, buffer), offset, impl.core.capabilities().limits.maxAddrOffset);
        // The Null encoding is D3D12's, not a virtual address: the buffer's slot index and the byte offset.
        return Addr{ core::HandlePool<Buffer, core::BufferRecord>::slotIndex(buffer), static_cast<uint32_t>(offset) };
    }

    //==================================================================================================================
    std::byte* Device::cpuPointer(Buffer buffer)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::BufferRecord& record = core::checkedBuffer(impl.core, buffer);
        core::checkCpuAccess(record);
        return static_cast<std::byte*>(record.native);
    }

    //==================================================================================================================
    void Device::destroy(Buffer buffer)
    {
        core::destroyBuffer(DeviceImpl::of(*this).core, buffer);
    }

    //==================================================================================================================
    void Device::destroy(Texture texture)
    {
        core::destroyTexture(DeviceImpl::of(*this).core, texture);
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
    void Device::destroy(Heap heap)
    {
        core::destroyHeap(DeviceImpl::of(*this).core, heap);
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
    Result<Fence> Device::createFence(uint64_t initial, std::string_view label)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        // A Null fence owns no native object beyond its record.
        return impl.core.pool<Fence>().allocate(core::prepareFence(initial, label));
    }

    //==================================================================================================================
    uint64_t Device::completedValue(Fence fence)
    {
        return core::completedValue(DeviceImpl::of(*this).core, fence);
    }

    //==================================================================================================================
    bool Device::wait(FenceValue value, uint64_t timeoutNs)
    {
        // Work completes during Queue::submit, so a value not reached now is never reached by waiting.
        return core::checkWait(DeviceImpl::of(*this).core, value, timeoutNs);
    }

    //==================================================================================================================
    void Device::destroy(Fence fence)
    {
        DeviceImpl::of(*this).core.destroy(fence);
    }

    //==================================================================================================================
    void Device::destroy(Upscaler upscaler)
    {
        DeviceImpl::of(*this).core.destroy(upscaler);
    }

    //==================================================================================================================
    void Device::destroy(ViewIndex view)
    {
        core::destroyView(DeviceImpl::of(*this).core, view);
    }

    //==================================================================================================================
    void Device::destroy(SamplerIndex sampler)
    {
        core::destroySampler(DeviceImpl::of(*this).core, sampler);
    }

    // Swapchains do not exist yet, so releasing one has nothing to defer.

    //==================================================================================================================
    void Device::destroy(Swapchain swapchain)
    {
        DeviceImpl::of(*this);
        core::checkObject(swapchain.m_impl, "Swapchain");
    }
} // namespace rojoRHI
