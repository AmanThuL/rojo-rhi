//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Stager.cpp
/// @brief Implements the D3D12 stager, which copies a resource's initial data on the device's internal upload queue.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Stager.h"

#include <rojoRHI/RHI.h>

#include "Base/Assert.h"
#include "Base/Log.h"
#include "Core/Counters.h"
#include "D3D12Common.h"
#include "D3D12Debug.h"
#include "D3D12Device.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rojoRHI::d3d12 {
    namespace {
        /// The label of the upload queue and of everything it records and signals with.
        constexpr std::string_view kQueueLabel = "rojorhi.upload";

        /// The label of every staging buffer.
        constexpr std::string_view kStagingLabel = "rojorhi.staging";

        /// `D3D12_BARRIER_SUBRESOURCE_RANGE::IndexOrFirstMipLevel` with `NumMipLevels` 0: every subresource of the texture.
        constexpr UINT kAllSubresources = 0xFFFFFFFF;

        /// A committed `UPLOAD` buffer, mapped for writing until it is released.
        struct Staging
        {
            Com<ID3D12Resource> buffer; ///< The staging buffer.
            std::byte* host = nullptr;  ///< Its mapped bytes.
        };

        //==============================================================================================================
        /// Creates a committed `UPLOAD` buffer of `size` bytes labelled `rojorhi.staging` on `impl`'s device and maps it,
        /// counting one `nativeCreateCalls` and one `nativeHostCalls`.
        Result<Staging> createStaging(DeviceImpl& impl, uint64_t size)
        {
            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC1 desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = size;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            Staging staging;
            const HRESULT created = impl.device()->CreateCommittedResource3(
                &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(staging.buffer.put())
            );
            if (FAILED(created))
            {
                const std::string what = std::format("ID3D12Device10::CreateCommittedResource3 of {} ({} bytes) for {}", kStagingLabel, size, kQueueLabel);
                return std::unexpected(failure(impl, created, what));
            }
            core::DeviceCounts::add(impl.core.counts().nativeCreateCalls);
            setName(staging.buffer.get(), kStagingLabel);
            // An empty read range: the CPU only writes the staging buffer.
            const D3D12_RANGE read = { 0, 0 };
            void* mapped = nullptr;
            const HRESULT mappedResult = staging.buffer->Map(0, &read, &mapped);
            core::DeviceCounts::add(impl.core.counts().nativeHostCalls);
            if (FAILED(mappedResult))
            {
                return std::unexpected(failure(impl, mappedResult, std::format("ID3D12Resource::Map of {} for {}", kStagingLabel, kQueueLabel)));
            }
            staging.host = static_cast<std::byte*>(mapped);
            return staging;
        }

        //==============================================================================================================
        /// Unmaps the staging buffer of `staging`, if it has one, before its release.
        void unmap(Staging& staging)
        {
            if (staging.buffer)
            {
                staging.buffer->Unmap(0, nullptr);
            }
        }

        //==============================================================================================================
        /// Records into `list` the copy of each non-empty entry of `subresources` from `staging`, laid out as `footprints`
        /// says, into the same subresource of `destination`, then the barrier that moves every subresource of `destination`
        /// from COPY_DEST to SHADER_RESOURCE.
        void recordTexture(
            ID3D12GraphicsCommandList9* list,
            ID3D12Resource* destination,
            ID3D12Resource* staging,
            std::span<const SubresourceData> subresources,
            std::span<const D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints
        )
        {
            for (size_t index = 0; index < subresources.size(); ++index)
            {
                if (subresources[index].bytes.empty())
                {
                    continue;
                }
                D3D12_TEXTURE_COPY_LOCATION target = {};
                target.pResource = destination;
                target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                target.SubresourceIndex = static_cast<UINT>(index);
                D3D12_TEXTURE_COPY_LOCATION source = {};
                source.pResource = staging;
                source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                source.PlacedFootprint = footprints[index];
                list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
            }
            // One barrier over every subresource, empty ones included, so none is left in COPY_DEST: the texture starts in
            // Layout::ShaderRead, which the generic SHADER_RESOURCE layout serves on direct and compute queues alike.
            D3D12_TEXTURE_BARRIER barrier = {};
            barrier.SyncBefore = D3D12_BARRIER_SYNC_COPY;
            barrier.SyncAfter = D3D12_BARRIER_SYNC_NONE;
            barrier.AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST;
            barrier.AccessAfter = D3D12_BARRIER_ACCESS_NO_ACCESS;
            barrier.LayoutBefore = D3D12_BARRIER_LAYOUT_COPY_DEST;
            barrier.LayoutAfter = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
            barrier.pResource = destination;
            barrier.Subresources.IndexOrFirstMipLevel = kAllSubresources;
            barrier.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;
            D3D12_BARRIER_GROUP group = {};
            group.Type = D3D12_BARRIER_TYPE_TEXTURE;
            group.NumBarriers = 1;
            group.pTextureBarriers = &barrier;
            list->Barrier(1, &group);
        }
    } // namespace

    //==================================================================================================================
    Result<void> Stager::create(DeviceImpl& impl)
    {
        ID3D12Device12* device = impl.device();
        D3D12_COMMAND_QUEUE_DESC desc = {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        if (const HRESULT created = device->CreateCommandQueue(&desc, IID_PPV_ARGS(m_queue.put())); FAILED(created))
        {
            return std::unexpected(failure(impl, created, std::format("ID3D12Device::CreateCommandQueue for {}", kQueueLabel)));
        }
        setName(m_queue.get(), kQueueLabel);
        if (const HRESULT created = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(m_allocator.put())); FAILED(created))
        {
            return std::unexpected(failure(impl, created, std::format("ID3D12Device::CreateCommandAllocator for {}", kQueueLabel)));
        }
        setName(m_allocator.get(), kQueueLabel);
        // CreateCommandList1 makes the list closed, so each upload starts with the same Reset.
        if (const HRESULT created = device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(m_list.put()));
            FAILED(created))
        {
            return std::unexpected(failure(impl, created, std::format("ID3D12Device4::CreateCommandList1 for {}", kQueueLabel)));
        }
        setName(m_list.get(), kQueueLabel);
        if (const HRESULT created = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(m_fence.put())); FAILED(created))
        {
            return std::unexpected(failure(impl, created, std::format("ID3D12Device::CreateFence for {}", kQueueLabel)));
        }
        setName(m_fence.get(), kQueueLabel);
        return {};
    }

    //==================================================================================================================
    Result<void> Stager::uploadBuffer(DeviceImpl& impl, ID3D12Resource* destination, std::span<const std::byte> bytes)
    {
        ROJORHI_INTERNAL_ASSERT(destination != nullptr, "uploadBuffer was given no destination");
        ROJORHI_INTERNAL_ASSERT(!bytes.empty(), "uploadBuffer was given no bytes; the creation entry stages only non-empty data");
        const std::lock_guard lock(m_mutex);
        Result<Staging> staging = createStaging(impl, bytes.size());
        if (!staging)
        {
            return std::unexpected(std::move(staging.error()));
        }
        std::memcpy(staging->host, bytes.data(), bytes.size());
        Result<void> copied = reset(impl);
        if (copied)
        {
            m_list->CopyBufferRegion(destination, 0, staging->buffer.get(), 0, bytes.size());
            copied = submit(impl);
        }
        // `submit` waited for the copy, or failed before the list ran, so the staging buffer is free to go.
        unmap(*staging);
        return copied;
    }

    //==================================================================================================================
    Result<void> Stager::uploadTexture(DeviceImpl& impl, ID3D12Resource* destination, std::span<const SubresourceData> subresources)
    {
        ROJORHI_INTERNAL_ASSERT(destination != nullptr, "uploadTexture was given no destination");
        ROJORHI_INTERNAL_ASSERT(!subresources.empty(), "uploadTexture was given no subresources");
        Com<ID3D12Resource2> described;
        if (const HRESULT queried = destination->QueryInterface(IID_PPV_ARGS(described.put())); FAILED(queried))
        {
            return std::unexpected(failure(impl, queried, std::format("ID3D12Resource::QueryInterface of ID3D12Resource2 for {}", kQueueLabel)));
        }
        const D3D12_RESOURCE_DESC1 desc = described->GetDesc1();
        ROJORHI_INTERNAL_ASSERT(
            subresources.size() ==
                size_t{ desc.MipLevels } * (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? size_t{ 1 } : size_t{ desc.DepthOrArraySize }),
            "uploadTexture was given other than one entry per mip and layer"
        );
        const auto count = static_cast<UINT>(subresources.size());
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
        std::vector<UINT> rows(count);
        std::vector<UINT64> rowSizes(count);
        UINT64 total = 0;
        impl.device()->GetCopyableFootprints1(&desc, 0, count, 0, footprints.data(), rows.data(), rowSizes.data(), &total);
        if (total == UINT64_MAX)
        {
            return std::unexpected(
                Error{ ErrorCategory::DriverFailure, std::format("ID3D12Device8::GetCopyableFootprints1 refused the texture for {}", kQueueLabel) }
            );
        }
        const bool anyData = std::ranges::any_of(subresources, [](const SubresourceData& data) { return !data.bytes.empty(); });
        const std::lock_guard lock(m_mutex);
        Staging staging;
        if (anyData)
        {
            Result<Staging> created = createStaging(impl, total);
            if (!created)
            {
                return std::unexpected(std::move(created.error()));
            }
            staging = std::move(*created);
            // Rows repacked from the caller's strides to the footprint's RowPitch, a multiple of 256; a depth slice of the
            // footprint is `rows` rows of RowPitch bytes.
            for (UINT index = 0; index < count; ++index)
            {
                const SubresourceData& data = subresources[index];
                if (data.bytes.empty())
                {
                    continue;
                }
                const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = footprints[index];
                const uint64_t rowPitch = footprint.Footprint.RowPitch;
                const uint64_t slicePitch = rowPitch * rows[index];
                ROJORHI_INTERNAL_ASSERT(
                    data.bytesPerImage * (footprint.Footprint.Depth - 1) + data.bytesPerRow * (rows[index] - 1) + rowSizes[index] <= data.bytes.size(),
                    "a subresource's bytes are shorter than its strides imply"
                );
                for (uint64_t slice = 0; slice < footprint.Footprint.Depth; ++slice)
                {
                    for (uint64_t row = 0; row < rows[index]; ++row)
                    {
                        std::memcpy(
                            staging.host + footprint.Offset + slice * slicePitch + row * rowPitch,
                            data.bytes.data() + slice * data.bytesPerImage + row * data.bytesPerRow,
                            rowSizes[index]
                        );
                    }
                }
            }
        }
        Result<void> copied = reset(impl);
        if (copied)
        {
            recordTexture(m_list.get(), destination, staging.buffer.get(), subresources, footprints);
            copied = submit(impl);
        }
        unmap(staging);
        return copied;
    }

    //==================================================================================================================
    void Stager::idle()
    {
        const std::lock_guard lock(m_mutex);
        if (m_fence && m_value != 0)
        {
            // Teardown's own wait, not counted: no counter is read after the device is destroyed.
            (void)waitForFence(m_fence.get(), m_value, UINT64_MAX, kQueueLabel);
        }
    }

    //==================================================================================================================
    Result<void> Stager::reset(DeviceImpl& impl)
    {
        if (const HRESULT cleared = m_allocator->Reset(); FAILED(cleared))
        {
            return std::unexpected(failure(impl, cleared, std::format("ID3D12CommandAllocator::Reset for {}", kQueueLabel)));
        }
        if (const HRESULT opened = m_list->Reset(m_allocator.get(), nullptr); FAILED(opened))
        {
            return std::unexpected(failure(impl, opened, std::format("ID3D12GraphicsCommandList::Reset for {}", kQueueLabel)));
        }
        return {};
    }

    //==================================================================================================================
    Result<void> Stager::submit(DeviceImpl& impl)
    {
        if (const HRESULT closed = m_list->Close(); FAILED(closed))
        {
            return std::unexpected(failure(impl, closed, std::format("ID3D12GraphicsCommandList::Close for {}", kQueueLabel)));
        }
        ID3D12CommandList* const lists[] = { m_list.get() };
        m_queue->ExecuteCommandLists(1, lists);
        core::DeviceCounts::add(impl.core.counts().nativeSubmitCalls);
        // The value advances only once the signal is queued, so `idle` never waits for a value no signal will reach.
        const uint64_t value = m_value + 1;
        const HRESULT signalled = m_queue->Signal(m_fence.get(), value);
        core::DeviceCounts::add(impl.core.counts().nativeSubmitCalls);
        if (FAILED(signalled))
        {
            // The list may still run, and nothing can wait for it: releasing its staging buffer or resetting its allocator
            // then is undefined. Only a removed device, whose work no longer runs, returns the error.
            if (!deviceRemoved(signalled))
            {
                ROJORHI_LOG_ERROR(
                    "ID3D12CommandQueue::Signal for {} failed with HRESULT 0x{:08X} after its copy was submitted", kQueueLabel, hresultBits(signalled)
                );
                std::abort();
            }
            return std::unexpected(failure(impl, signalled, std::format("ID3D12CommandQueue::Signal for {}", kQueueLabel)));
        }
        m_value = value;
        const FenceWait waited = waitForFence(m_fence.get(), value, UINT64_MAX, kQueueLabel);
        core::DeviceCounts::add(impl.core.counts().nativeHostCalls, waited.hostCalls);
        // A removed device completes every fence value at once, so the wait alone cannot tell a copy that ran from one that
        // never will.
        if (const HRESULT removed = impl.device()->GetDeviceRemovedReason(); FAILED(removed))
        {
            return std::unexpected(failure(impl, removed, std::format("the copy on {}", kQueueLabel)));
        }
        return {};
    }
} // namespace rojoRHI::d3d12
