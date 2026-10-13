//----------------------------------------------------------------------------------------------------------------------
/// @file NullCommandList.cpp
/// @brief Implements the public command list entries for the Null backend: lists keep a state, frame data lives in host buffers.
//----------------------------------------------------------------------------------------------------------------------

#include "NullDevice.h"

#include <rojoRHI/CommandList.h>
#include <rojoRHI/Device.h>

#include "Base/Log.h"
#include "Core/Buffers.h"
#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/FrameArena.h"

#include <cstdlib>
#include <cstring>
#include <format>
#include <utility>

namespace rojoRHI {
    namespace {
        //==============================================================================================================
        /// Creates one arena page as a zeroed host buffer of the pool, so an address into it resolves like any buffer's.
        Result<core::ArenaPage> createArenaPage(void* context, uint64_t size)
        {
            core::DeviceCore& core = *static_cast<core::DeviceCore*>(context);
            core::BufferRecord record =
                core::prepareBuffer(core, BufferDesc{ .size = size, .memory = Memory::Upload, .label = "rojorhi.frameArena.page" }, {}, {}, {});
            auto* host = static_cast<std::byte*>(std::calloc(size, 1));
            if (host == nullptr)
            {
                return std::unexpected(Error{ ErrorCategory::OutOfMemory, std::format("no host memory for {} bytes of a frame arena page", size) });
            }
            record.native = host;
            return core::ArenaPage{ core::addBuffer(core, std::move(record)), host, size };
        }
    } // namespace

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

    namespace core {
        //==============================================================================================================
        CommandListCore& coreOf(CommandList list)
        {
            return CommandListImpl::of(list).core;
        }
    } // namespace core

    //==================================================================================================================
    CommandList Device::acquireCommandList(QueueType type, std::string_view label)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::checkAcquire(impl.core);
        ListPool& pool = impl.listPools[impl.core.frameNumber() % impl.core.framesInFlight()];
        CommandListImpl* list = nullptr;
        {
            std::lock_guard lock(pool.mutex);
            if (pool.used == pool.lists.size())
            {
                pool.lists.push_back(std::make_unique<CommandListImpl>());
            }
            list = pool.lists[pool.used++].get();
        }
        core::beginList(list->core, impl.core, type, label);
        return CommandListImpl::wrap(list);
    }

    //==================================================================================================================
    void CommandList::end()
    {
        core::endList(CommandListImpl::of(*this).core);
    }

    //==================================================================================================================
    Addr CommandList::uploadFrameData(std::span<const std::byte> bytes, uint32_t alignment)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        const core::ArenaAllocation place = core::allocateFrameData(*list.device, list, bytes, alignment, createArenaPage, list.device);
        std::memcpy(place.memory, bytes.data(), bytes.size());
        // The Null encoding, as Device::address makes it: the page's slot index and the byte offset in it.
        return Addr{ core::HandlePool<Buffer, core::BufferRecord>::slotIndex(place.page), static_cast<uint32_t>(place.offset) };
    }
} // namespace rojoRHI
