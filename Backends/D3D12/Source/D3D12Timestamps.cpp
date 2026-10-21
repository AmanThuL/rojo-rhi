//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Timestamps.cpp
/// @brief Implements the D3D12 timestamp queries of a list (claims, heap growth, the resolve at end) and the frame wait's durations.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Timestamps.h"

#include <rojoRHI/RHI.h>

#include "Base/Log.h"
#include "Core/CommandLists.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/ScopeRecord.h"
#include "D3D12Common.h"
#include "D3D12Debug.h"
#include "D3D12Device.h"
#include "D3D12ListState.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <span>
#include <string>
#include <string_view>

namespace rojoRHI::d3d12 {
    namespace {
        /// The name of each queue type in messages and native names, indexed by `QueueType`.
        constexpr std::array<std::string_view, 3> kTypeNames = { "graphics", "compute", "copy" };

        //==============================================================================================================
        /// Returns whether a list of `type` on `impl` writes timestamps: every Graphics and Compute list, and a Copy list when
        /// the device reports copy-queue timestamps.
        bool writesTimestamps(const DeviceImpl& impl, QueueType type)
        {
            return type != QueueType::Copy || impl.features.copyQueueTimestamps;
        }

        //==============================================================================================================
        /// Makes heap number `number` of `list`'s timestamps and its readback buffer on `impl`, names both after the list's
        /// pool entry, maps the buffer and returns them.
        ListTimestamps::Heap createHeap(DeviceImpl& impl, const ListState& state, size_t number)
        {
            const std::string name =
                std::format("rojorhi.list.{}.{}.{}.timestamps.{}", state.frameSlot, kTypeNames[static_cast<size_t>(state.type)], state.ordinal, number);
            ListTimestamps::Heap heap;
            D3D12_QUERY_HEAP_DESC queries = {};
            queries.Type = state.type == QueueType::Copy ? D3D12_QUERY_HEAP_TYPE_COPY_QUEUE_TIMESTAMP : D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
            queries.Count = ListTimestamps::kHeapQueries;
            queries.NodeMask = 0;
            requireSucceeded(
                impl, impl.device()->CreateQueryHeap(&queries, IID_PPV_ARGS(heap.queries.put())), std::format("ID3D12Device::CreateQueryHeap for {}", name)
            );
            core::DeviceCounts::add(impl.core.counts().nativeCreateCalls);
            setName(heap.queries.get(), name);
            D3D12_HEAP_PROPERTIES properties = {};
            properties.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC1 desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = uint64_t{ ListTimestamps::kHeapQueries } * sizeof(uint64_t);
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            const std::string readback = name + ".readback";
            requireSucceeded(
                impl,
                impl.device()->CreateCommittedResource3(
                    &properties, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(heap.readback.put())
                ),
                std::format("ID3D12Device10::CreateCommittedResource3 of {}", readback)
            );
            core::DeviceCounts::add(impl.core.counts().nativeCreateCalls);
            core::DeviceCounts::add(impl.core.counts().gpuAllocations);
            setName(heap.readback.get(), readback);
            // The buffer stays mapped: the CPU reads it only at the frame wait, after the GPU's resolve into it completed.
            void* mapped = nullptr;
            requireSucceeded(impl, heap.readback->Map(0, nullptr, &mapped), std::format("ID3D12Resource::Map of {}", readback));
            core::DeviceCounts::add(impl.core.counts().nativeHostCalls);
            heap.mapped = static_cast<const uint64_t*>(mapped);
            return heap;
        }
    } // namespace

    //==================================================================================================================
    Result<void> Timestamps::create(DeviceImpl& impl)
    {
        for (size_t i = 0; i < m_frequency.size(); ++i)
        {
            if (!writesTimestamps(impl, static_cast<QueueType>(i)))
            {
                continue;
            }
            UINT64 ticks = 0;
            if (const HRESULT read = impl.queues[i].native->GetTimestampFrequency(&ticks); FAILED(read))
            {
                return std::unexpected(failure(impl, read, std::format("ID3D12CommandQueue::GetTimestampFrequency on the {} queue", kTypeNames[i])));
            }
            m_frequency[i] = static_cast<double>(ticks);
        }
        return {};
    }

    //==================================================================================================================
    void Timestamps::release()
    {
    }

    //==================================================================================================================
    double Timestamps::frequency(QueueType type) const
    {
        return m_frequency[static_cast<size_t>(type)];
    }

    //==================================================================================================================
    void Timestamps::resolve(DeviceImpl& impl, uint32_t slot, std::span<const core::ScopeRecord> records, std::span<ScopeTiming> report)
    {
        const QueueType queue = report.front().queue;
        const double ticksPerSecond = m_frequency[static_cast<size_t>(queue)];
        // A queue's records come list by list, so the pool, whose lookup takes its mutex, is asked once per list.
        const ListTimestamps* timestamps = nullptr;
        uint32_t ordinal = core::kNoTimestamp;
        uint64_t untimed = 0;
        for (size_t i = 0; i < records.size(); ++i)
        {
            const uint32_t timestamp = records[i].timestamp;
            if (timestamp == core::kNoTimestamp)
            {
                ++untimed;
                continue;
            }
            if ((timestamp >> 16) != ordinal)
            {
                ordinal = timestamp >> 16;
                timestamps = &impl.lists.at(slot, queue, ordinal).state.timestamps;
            }
            report[i].gpuMilliseconds = timestamps->milliseconds(timestamp & 0xFFFF, ticksPerSecond);
        }
        const uint64_t frame = report.front().frame;
        if (untimed == 0 || frame == m_warnedFrame)
        {
            return;
        }
        m_warnedFrame = frame;
        ROJORHI_LOG_WARN(
            "frame {}: {} scope(s) on the {} queue report no GPU time; {}",
            frame,
            untimed,
            kTypeNames[static_cast<size_t>(queue)],
            writesTimestamps(impl, queue) ? std::format(
                                                "a list claimed more than its {} timestamp queries or its pool held more than {} lists",
                                                ListTimestamps::kMaxQueries,
                                                ListTimestamps::kMaxOrdinal + 1
                                            )
                                          : std::string("the device writes no timestamps on its copy queue")
        );
    }

    //==================================================================================================================
    uint32_t ListTimestamps::claimPair(CommandListImpl& list)
    {
        const ListState& state = list.state;
        // A device created without scope timings claims nothing, so its lists make no query heap or readback buffer and
        // resolve nothing at `end`.
        const DeviceImpl& device = deviceOf(list);
        if (!device.core.timesScopes() || next + 2 > kMaxQueries || state.ordinal > kMaxOrdinal || !writesTimestamps(device, state.type))
        {
            return core::kNoTimestamp;
        }
        const uint32_t pair = next;
        if (pair / kHeapQueries == heaps.size())
        {
            // The heap is the list's own, so its creation takes no lock.
            heaps.push_back(createHeap(deviceOf(list), state, heaps.size()));
        }
        next += 2;
        return pair;
    }

    //==================================================================================================================
    void ListTimestamps::reset()
    {
        next = 0;
        open.clear();
    }

    //==================================================================================================================
    double ListTimestamps::milliseconds(uint32_t pair, double frequency) const
    {
        const size_t number = pair / kHeapQueries;
        if (number >= heaps.size() || frequency == 0.0)
        {
            return 0.0;
        }
        // The buffer stays mapped: READBACK memory is coherent for CPU reads once the frame fence shows the resolve complete.
        const uint64_t* ticks = heaps[number].mapped + pair % kHeapQueries;
        const uint64_t begin = ticks[0];
        const uint64_t end = ticks[1];
        if (end < begin)
        {
            return 0.0;
        }
        return static_cast<double>(end - begin) * 1000.0 / frequency;
    }

    //==================================================================================================================
    void resolveTimestamps(CommandListImpl& list)
    {
        const ListTimestamps& timestamps = list.state.timestamps;
        // Pairs fill the heaps in order, so every heap below the last claimed query was used, the last one up to it.
        for (uint32_t first = 0; first < timestamps.next; first += ListTimestamps::kHeapQueries)
        {
            const ListTimestamps::Heap& heap = timestamps.heaps[first / ListTimestamps::kHeapQueries];
            const uint32_t used = std::min(timestamps.next - first, ListTimestamps::kHeapQueries);
            list.state.list->ResolveQueryData(heap.queries.get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, used, heap.readback.get(), 0);
            core::noteNativeCall(list.core);
        }
    }

    //==================================================================================================================
    void scopeTimes(void* context, uint32_t slot, std::span<const core::ScopeRecord> records, std::span<ScopeTiming> report)
    {
        DeviceImpl& impl = *static_cast<DeviceImpl*>(context);
        impl.timestamps.resolve(impl, slot, records, report);
    }
} // namespace rojoRHI::d3d12
