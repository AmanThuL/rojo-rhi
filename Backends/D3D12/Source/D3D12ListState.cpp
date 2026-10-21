//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12ListState.cpp
/// @brief Implements the D3D12 command list pools, which grow when a frame slot runs out, and the restore after vendor work.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12ListState.h"

#include <rojoRHI/RHI.h>

#include "Base/Assert.h"
#include "Core/CommandLists.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "D3D12Common.h"
#include "D3D12Device.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace rojoRHI::d3d12 {
    namespace {
        /// The native list type of each queue type, indexed by `QueueType`.
        constexpr std::array<D3D12_COMMAND_LIST_TYPE, 3> kListTypes = { D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                        D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                                        D3D12_COMMAND_LIST_TYPE_COPY };

        /// The name of each queue type in a pooled list's native name, indexed by `QueueType`.
        constexpr std::array<std::string_view, 3> kTypeNames = { "graphics", "compute", "copy" };

        //==============================================================================================================
        /// Creates the allocator and the closed native list of `list`, the list of `type` at `ordinal` in frame slot `slot`'s
        /// pool on `impl`, names both and records where the list belongs.
        void createNative(DeviceImpl& impl, CommandListImpl& list, uint32_t slot, QueueType type, uint32_t ordinal)
        {
            const size_t index = static_cast<size_t>(type);
            const std::string name = std::format("rojorhi.list.{}.{}.{}", slot, kTypeNames[index], ordinal);
            ListState& state = list.state;
            requireSucceeded(
                impl,
                impl.device()->CreateCommandAllocator(kListTypes[index], IID_PPV_ARGS(state.allocator.put())),
                std::format("ID3D12Device::CreateCommandAllocator for {}", name)
            );
            core::DeviceCounts::add(impl.core.counts().nativeCreateCalls);
            setName(state.allocator.get(), name);
            // A list made by CreateCommandList1 starts closed, so every acquisition, the first included, resets it.
            requireSucceeded(
                impl,
                impl.device()->CreateCommandList1(0, kListTypes[index], D3D12_COMMAND_LIST_FLAG_NONE, IID_PPV_ARGS(state.list.put())),
                std::format("ID3D12Device4::CreateCommandList1 for {}", name)
            );
            core::DeviceCounts::add(impl.core.counts().nativeCreateCalls);
            setName(state.list.get(), name);
            state.type = type;
            state.frameSlot = slot;
            state.ordinal = ordinal;
            state.label = name;
        }
    } // namespace

    //==================================================================================================================
    CommandListImpl& ListPools::acquire(DeviceImpl& impl, uint32_t slot, QueueType type)
    {
        ROJORHI_INTERNAL_ASSERT(slot < m_slots.size(), "a frame slot beyond the three a device may use has no list pool");
        const size_t index = static_cast<size_t>(type);
        ROJORHI_INTERNAL_ASSERT(index < m_slots[slot].size(), "a QueueType outside the enumerators has no list pool");
        ListPool& pool = m_slots[slot][index];
        std::lock_guard lock(pool.mutex);
        if (pool.used == pool.lists.size())
        {
            auto created = std::make_unique<CommandListImpl>();
            core::DeviceCounts::add(impl.core.counts().cpuAllocations);
            createNative(impl, *created, slot, type, static_cast<uint32_t>(pool.lists.size()));
            pool.lists.push_back(std::move(created));
        }
        return *pool.lists[pool.used++];
    }

    //==================================================================================================================
    void ListPools::reset(DeviceImpl& impl, uint32_t slot)
    {
        ROJORHI_INTERNAL_ASSERT(slot < m_slots.size(), "a frame slot beyond the three a device may use has no list pool");
        for (ListPool& pool : m_slots[slot])
        {
            std::lock_guard lock(pool.mutex);
            // The frame wait before the slot's reuse proved every list the slot handed out complete on the GPU.
            for (size_t i = 0; i < pool.used; ++i)
            {
                requireSucceeded(impl, pool.lists[i]->state.allocator->Reset(), "ID3D12CommandAllocator::Reset at the reuse of a frame slot");
                core::DeviceCounts::add(impl.core.counts().nativeHostCalls);
            }
            pool.used = 0;
        }
    }

    //==================================================================================================================
    CommandListImpl& ListPools::at(uint32_t slot, QueueType type, uint32_t ordinal)
    {
        ROJORHI_INTERNAL_ASSERT(slot < m_slots.size(), "a frame slot beyond the three a device may use has no list pool");
        const size_t index = static_cast<size_t>(type);
        ROJORHI_INTERNAL_ASSERT(index < m_slots[slot].size(), "a QueueType outside the enumerators has no list pool");
        ListPool& pool = m_slots[slot][index];
        std::lock_guard lock(pool.mutex);
        ROJORHI_INTERNAL_ASSERT(ordinal < pool.lists.size(), "a list ordinal its frame slot's pool never made");
        return *pool.lists[ordinal];
    }

    //==================================================================================================================
    DeviceImpl& deviceOf(const CommandListImpl& list)
    {
        return *static_cast<DeviceImpl*>(list.core.device->retireHooks().context);
    }

    //==================================================================================================================
    void resumeAfterVendorWork(CommandListImpl& list)
    {
        ListState& state = list.state;
        const DeviceImpl& impl = deviceOf(list);
        // Shaders index both heaps directly, so the heaps are set before the root signatures the next commands set again.
        ID3D12DescriptorHeap* const heaps[] = { impl.descriptors.views(), impl.descriptors.samplers() };
        state.list->SetDescriptorHeaps(2, heaps);
        core::noteNativeCall(list.core);
        Binding& binding = state.binding;
        binding.graphicsSignatureDirty = true;
        binding.computeRoot = ComputeRoot::Foreign;
        binding.graphicsRootDirty = true;
        binding.computeRootDirty = true;
        binding.drawIdDirty = true;
        // One pipeline slot serves both sides, and neither holds a known pipeline now.
        binding.computePipeline = nullptr;
        binding.graphicsPipeline = nullptr;
        // Every draw after the vendor work follows a setPipeline in a later pass, which sets a topology the list does not hold.
        state.dynamic.topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    }
} // namespace rojoRHI::d3d12
