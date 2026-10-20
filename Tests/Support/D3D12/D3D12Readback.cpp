//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Readback.cpp
/// @brief Implements the test helpers that read any native D3D12 buffer or texture subresource back to the CPU.
//----------------------------------------------------------------------------------------------------------------------

#include "Support/D3D12/D3D12Readback.h"

#include "D3D12Common.h"
#include "D3D12Device.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace rojoRHI::test {
    namespace {
        /// The label of every object a readback makes.
        constexpr std::string_view kLabel = "test.readback";

        /// A DIRECT queue with one allocator, one list and one fence, made for one copy and released after it.
        struct OneShot
        {
            d3d12::Com<ID3D12CommandQueue> queue;         ///< The queue the copy runs on.
            d3d12::Com<ID3D12CommandAllocator> allocator; ///< The allocator `list` records into.
            d3d12::Com<ID3D12GraphicsCommandList9> list;  ///< The list, open for recording once `open` returns.
            d3d12::Com<ID3D12Fence> fence;                ///< Signalled with 1 when the copy has run.
        };

        //==============================================================================================================
        /// Returns a new queue, allocator, fence and list on `device`, the list open for recording. A failed native call fails
        /// the running case.
        OneShot open(ID3D12Device12* device)
        {
            OneShot shot;
            D3D12_COMMAND_QUEUE_DESC desc = {};
            desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            REQUIRE(SUCCEEDED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(shot.queue.put()))));
            d3d12::setName(shot.queue.get(), kLabel);
            REQUIRE(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(shot.allocator.put()))));
            d3d12::setName(shot.allocator.get(), kLabel);
            REQUIRE(SUCCEEDED(device->CreateCommandList1(0, D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(shot.list.put()))));
            d3d12::setName(shot.list.get(), kLabel);
            REQUIRE(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(shot.fence.put()))));
            d3d12::setName(shot.fence.get(), kLabel);
            REQUIRE(SUCCEEDED(shot.list->Reset(shot.allocator.get(), nullptr)));
            return shot;
        }

        //==============================================================================================================
        /// Closes the list of `shot`, executes it, signals its fence with 1 and waits for the value, then requires that
        /// `device`, which a removal would complete the wait for at once, was not removed.
        void runAndWait(ID3D12Device12* device, OneShot& shot)
        {
            REQUIRE(SUCCEEDED(shot.list->Close()));
            ID3D12CommandList* const lists[] = { shot.list.get() };
            shot.queue->ExecuteCommandLists(1, lists);
            REQUIRE(SUCCEEDED(shot.queue->Signal(shot.fence.get(), 1)));
            REQUIRE(d3d12::waitForFence(shot.fence.get(), 1, UINT64_MAX, kLabel).reached);
            REQUIRE(SUCCEEDED(device->GetDeviceRemovedReason()));
        }

        //==============================================================================================================
        /// Returns a new committed `READBACK` buffer of `size` bytes on `device`.
        d3d12::Com<ID3D12Resource> createReadback(ID3D12Device12* device, uint64_t size)
        {
            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC1 desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = size;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            d3d12::Com<ID3D12Resource> buffer;
            REQUIRE(SUCCEEDED(device->CreateCommittedResource3(
                &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(buffer.put())
            )));
            d3d12::setName(buffer.get(), kLabel);
            return buffer;
        }

        //==============================================================================================================
        /// Returns a barrier on subresource `subresource` of `texture` from `before` to `after`: one that waits for nothing
        /// and makes the subresource a copy source when `after` is `D3D12_BARRIER_LAYOUT_COPY_SOURCE`, else one that waits for
        /// the copy and leaves no access behind.
        D3D12_TEXTURE_BARRIER layoutBarrier(ID3D12Resource* texture, uint32_t subresource, D3D12_BARRIER_LAYOUT before, D3D12_BARRIER_LAYOUT after)
        {
            const bool toCopy = after == D3D12_BARRIER_LAYOUT_COPY_SOURCE;
            D3D12_TEXTURE_BARRIER barrier = {};
            barrier.SyncBefore = toCopy ? D3D12_BARRIER_SYNC_NONE : D3D12_BARRIER_SYNC_COPY;
            barrier.SyncAfter = toCopy ? D3D12_BARRIER_SYNC_COPY : D3D12_BARRIER_SYNC_NONE;
            barrier.AccessBefore = toCopy ? D3D12_BARRIER_ACCESS_NO_ACCESS : D3D12_BARRIER_ACCESS_COPY_SOURCE;
            barrier.AccessAfter = toCopy ? D3D12_BARRIER_ACCESS_COPY_SOURCE : D3D12_BARRIER_ACCESS_NO_ACCESS;
            barrier.LayoutBefore = before;
            barrier.LayoutAfter = after;
            barrier.pResource = texture;
            // NumMipLevels 0 makes IndexOrFirstMipLevel a subresource index.
            barrier.Subresources.IndexOrFirstMipLevel = subresource;
            barrier.Flags = D3D12_TEXTURE_BARRIER_FLAG_NONE;
            return barrier;
        }

        //==============================================================================================================
        /// Records `barrier` into `list` as one barrier group.
        void recordBarrier(ID3D12GraphicsCommandList9* list, const D3D12_TEXTURE_BARRIER& barrier)
        {
            D3D12_BARRIER_GROUP group = {};
            group.Type = D3D12_BARRIER_TYPE_TEXTURE;
            group.NumBarriers = 1;
            group.pTextureBarriers = &barrier;
            list->Barrier(1, &group);
        }
    } // namespace

    //==================================================================================================================
    std::vector<std::byte> readNativeBuffer(DeviceImpl& impl, ID3D12Resource* buffer, uint64_t size)
    {
        REQUIRE(buffer != nullptr);
        ID3D12Device12* device = impl.device();
        d3d12::Com<ID3D12Resource> readback = createReadback(device, size);
        OneShot shot = open(device);
        shot.list->CopyBufferRegion(readback.get(), 0, buffer, 0, size);
        runAndWait(device, shot);
        void* mapped = nullptr;
        const D3D12_RANGE read = { 0, static_cast<SIZE_T>(size) };
        REQUIRE(SUCCEEDED(readback->Map(0, &read, &mapped)));
        std::vector<std::byte> bytes(size);
        std::memcpy(bytes.data(), mapped, bytes.size());
        const D3D12_RANGE written = { 0, 0 };
        readback->Unmap(0, &written);
        return bytes;
    }

    //==================================================================================================================
    std::vector<std::byte> readNativeTexture(DeviceImpl& impl, ID3D12Resource* texture, uint32_t subresource, D3D12_BARRIER_LAYOUT layout)
    {
        REQUIRE(texture != nullptr);
        ID3D12Device12* device = impl.device();
        d3d12::Com<ID3D12Resource2> described;
        REQUIRE(SUCCEEDED(texture->QueryInterface(IID_PPV_ARGS(described.put()))));
        const D3D12_RESOURCE_DESC1 desc = described->GetDesc1();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT rows = 0;
        UINT64 rowSize = 0;
        UINT64 total = 0;
        device->GetCopyableFootprints1(&desc, subresource, 1, 0, &footprint, &rows, &rowSize, &total);
        REQUIRE(total != UINT64_MAX);
        d3d12::Com<ID3D12Resource> readback = createReadback(device, total);
        OneShot shot = open(device);
        recordBarrier(shot.list.get(), layoutBarrier(texture, subresource, layout, D3D12_BARRIER_LAYOUT_COPY_SOURCE));
        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = readback.get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = texture;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = subresource;
        shot.list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        recordBarrier(shot.list.get(), layoutBarrier(texture, subresource, D3D12_BARRIER_LAYOUT_COPY_SOURCE, layout));
        runAndWait(device, shot);
        void* mapped = nullptr;
        const D3D12_RANGE read = { 0, static_cast<SIZE_T>(total) };
        REQUIRE(SUCCEEDED(readback->Map(0, &read, &mapped)));
        // A depth slice of the footprint is `rows` rows of `RowPitch` bytes; the result keeps `rowSize` bytes of each row.
        const auto* placed = static_cast<const std::byte*>(mapped) + footprint.Offset;
        const uint64_t rowPitch = footprint.Footprint.RowPitch;
        const uint64_t slicePitch = rowPitch * rows;
        std::vector<std::byte> bytes(rowSize * rows * footprint.Footprint.Depth);
        std::byte* out = bytes.data();
        for (uint64_t slice = 0; slice < footprint.Footprint.Depth; ++slice)
        {
            for (uint64_t row = 0; row < rows; ++row)
            {
                std::memcpy(out, placed + slice * slicePitch + row * rowPitch, rowSize);
                out += rowSize;
            }
        }
        const D3D12_RANGE written = { 0, 0 };
        readback->Unmap(0, &written);
        return bytes;
    }
} // namespace rojoRHI::test
