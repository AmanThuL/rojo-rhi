//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4CommandList.cpp
/// @brief Implements the public command list entries for the Metal 4 backend: lists record into command buffers, frame data lives in arena pages.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Device.h"

#include <rojoRHI/RHI.h>

#include "Base/Log.h"
#include "Core/Accesses.h"
#include "Core/Buffers.h"
#include "Core/CommandLists.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/FrameArena.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "Metal4Barriers.h"
#include "Metal4Buffers.h"
#include "Metal4Common.h"
#include "Metal4Memory.h"

#include <Metal/Metal.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>

namespace rojoRHI {

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

    namespace metal4 {
        //==============================================================================================================
        Result<core::ArenaPage> createArenaPage(void* context, uint64_t size)
        {
            core::DeviceCore& core = *static_cast<core::DeviceCore*>(context);
            DeviceImpl& impl = *static_cast<DeviceImpl*>(core.retireHooks().context);
            const AutoreleasePool pool;
            const BufferDesc desc{ .size = size, .memory = Memory::Upload, .usage = BufferUsage::IndirectArgs, .label = "rojorhi.frameArena.page" };
            core::BufferRecord record = core::prepareBuffer(core, desc, {}, {}, {});
            MTL::Buffer* buffer = impl.device->newBuffer(size, resourceOptions(Memory::Upload));
            if (buffer == nullptr)
            {
                return std::unexpected(Error{ ErrorCategory::OutOfMemory, std::format("no device memory for {} bytes of a frame arena page", size) });
            }
            buffer->setLabel(makeString(record.label));
            // The page joins the device set inside the arena's claim, the one lock recording may take (ADR 0020).
            impl.residency.add(buffer);
            std::unique_ptr<BufferPayload> payload = makeBufferPayload(core, buffer);
            record.native = payload.get();
            Result<Buffer> page = core::addFrameArenaPage(core, std::move(record));
            if (!page)
            {
                impl.residency.remove(buffer);
                buffer->release();
                return std::unexpected(std::move(page.error()));
            }
            // The pool's record owns the payload now; the retire hook frees it.
            const BufferPayload* owned = payload.release();
#ifndef NDEBUG
            // Only Debug checks decode addresses, so only Debug builds keep the address map.
            impl.addresses.insert(owned->address, size, core::HandlePool<Buffer, core::BufferRecord>::slotIndex(*page), buffer);
#endif
            // The page keeps its GPU address, so an upload adds its offset to it and sends the page no message.
            return core::ArenaPage{ .buffer = *page, .host = static_cast<std::byte*>(buffer->contents()), .size = size, .address = owned->address };
        }
    } // namespace metal4

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
                auto created = std::make_unique<CommandListImpl>();
                core::DeviceCounts::add(impl.core.counts().cpuAllocations);
                // The list's objects are its own for good: the pool keeps the list, and the slot's reuse resets its allocator.
                if (Result<void> made = created->state.create(
                        impl.device, impl.viewTable->gpuAddress(), impl.samplerTable->gpuAddress(), impl.commandPools, impl.encoderLabels
                    );
                    !made)
                {
                    ROJORHI_LOG_FATAL("acquireCommandList on device \"{}\": {}", impl.core.label(), made.error().message);
                    std::abort();
                }
                core::DeviceCounts::add(impl.core.counts().nativeCreateCalls, 4);
                pool.lists.push_back(std::move(created));
            }
            list = pool.lists[pool.used++].get();
        }
        core::beginList(list->core, impl.core, type, label);
        metal4::ListState& state = list->state;
        // A list that never ended, which only a Release build lets through, leaves its encoder behind; one that ended with
        // scopes open leaves their strings and timestamp pairs.
        list->encoders.release();
        if (type == QueueType::Graphics)
        {
            QueueImpl& graphics = impl.queues[static_cast<size_t>(QueueType::Graphics)];
            list->encoders.setCarry(graphics.carryFenced.load(std::memory_order_acquire) ? graphics.carry : nullptr, &graphics.kernelsRan);
        }
        else
        {
            list->encoders.setCarry(nullptr, nullptr);
        }
        // A stage no encoder of the queue runs names no work in a barrier: a graphics queue runs compute and render encoders, a
        // compute queue compute encoders, and a copy queue only the blits of its copies.
        MTL::Stages queueStages = metal4::kComputeStages | metal4::kRenderStages;
        if (type == QueueType::Compute)
        {
            queueStages = metal4::kComputeStages;
        }
        else if (type == QueueType::Copy)
        {
            queueStages = MTL::StageBlit;
        }
        list->encoders.setQueueStages(queueStages);
        list->draw.reset();
        for (NS::String* scopeLabel : state.scopeLabels)
        {
            scopeLabel->release();
        }
        state.scopeLabels.clear();
        state.scopeTimestamps.clear();
        state.groupCursor = 0;
        if (state.label == nullptr || state.labelText != list->core.label)
        {
            if (state.label != nullptr)
            {
                state.label->release();
            }
            // Made with alloc and init, so not autoreleased: a new label opens no pool to be made. It stays for encoder labels.
            state.label = NS::String::alloc()->init(list->core.label.c_str(), NS::UTF8StringEncoding);
            state.labelText = list->core.label;
            state.labelSet = false;
        }
        state.commandBuffer->beginCommandBuffer(state.allocator);
        core::noteNativeCall(list->core);
        // The command buffer keeps its label across recordings, so a list acquired again with the same label sends none.
        if (!state.labelSet)
        {
            {
                // The driver reads a short label through an autoreleased copy.
                const metal4::AutoreleasePool pool;
                state.commandBuffer->setLabel(state.label);
            }
            core::noteNativeCall(list->core);
            state.labelSet = true;
        }
        return CommandListImpl::wrap(list);
    }

    //==================================================================================================================
    void CommandList::end()
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        // The core's checks run before any native call.
        core::endList(list.core);
        // The driver autoreleases objects while it closes the open encoder and ends the buffer.
        const metal4::AutoreleasePool pool;
        // A barrier still pending orders the work of later lists on the queue: the open encoder carries it when it covers its
        // sources, and a trailing encoder emits what it did not carry.
        list.encoders.finish(list.core);
        // The list may wait in its pool long after its textures are destroyed; its descriptor must not keep them alive.
        list.draw.forgetTextures();
        metal4::emitTrailing(list.core, list.state, list.encoders.barriers(), list.encoders.carryFence());
        list.state.commandBuffer->endCommandBuffer();
        core::noteNativeCall(list.core);
    }

    //==================================================================================================================
    Addr CommandList::uploadFrameData(std::span<const std::byte> bytes, uint32_t alignment)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        // Only a list with no page, or a full one, takes the arena's lock to claim a page (ADR 0020).
        const core::ArenaAllocation place = core::allocateFrameData(*list.device, list, bytes, alignment, metal4::createArenaPage, list.device);
        std::memcpy(place.memory, bytes.data(), bytes.size());
        return Addr{ static_cast<uint32_t>(place.address), static_cast<uint32_t>(place.address >> 32) };
    }

    //==================================================================================================================
    void CommandList::declareAccessChecked(std::span<const ResourceAccess> accesses)
    {
        // A record for the validation tracker only: no native call.
        core::declareAccess(CommandListImpl::of(*this).core, accesses);
    }
} // namespace rojoRHI
