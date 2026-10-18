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
#include "Metal4Common.h"
#include "Metal4Memory.h"

#include <Metal/Metal.hpp>

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
            record.native = buffer;
            Result<Buffer> page = core::addFrameArenaPage(core, std::move(record));
            if (!page)
            {
                impl.residency.remove(buffer);
                buffer->release();
                return std::unexpected(std::move(page.error()));
            }
#ifndef NDEBUG
            // Only Debug checks decode addresses, so only Debug builds keep the address map.
            impl.addresses.insert(buffer->gpuAddress(), size, core::HandlePool<Buffer, core::BufferRecord>::slotIndex(*page), buffer);
#endif
            return core::ArenaPage{ *page, static_cast<std::byte*>(buffer->contents()), size };
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
                if (Result<void> made = created->state.create(impl.device, impl.viewTable->gpuAddress(), impl.samplerTable->gpuAddress()); !made)
                {
                    ROJORHI_LOG_ERROR("acquireCommandList on device \"{}\": {}", impl.core.label(), made.error().message);
                    std::abort();
                }
                core::DeviceCounts::add(impl.core.counts().nativeCreateCalls, 3);
                pool.lists.push_back(std::move(created));
            }
            list = pool.lists[pool.used++].get();
        }
        core::beginList(list->core, impl.core, type, label);
        metal4::ListState& state = list->state;
        // A list that never ended, which only a Release build lets through, leaves its encoder behind; one that ended with
        // scopes open leaves their strings.
        list->encoders.release();
        for (NS::String* scopeLabel : state.scopeLabels)
        {
            scopeLabel->release();
        }
        state.scopeLabels.clear();
        if (state.label != nullptr)
        {
            state.label->release();
        }
        // Made with alloc and init, so not autoreleased: acquisition opens no pool. The string stays for encoder labels.
        state.label = NS::String::alloc()->init(list->core.label.c_str(), NS::UTF8StringEncoding);
        state.commandBuffer->beginCommandBuffer(state.allocator);
        core::noteNativeCall(list->core);
        state.commandBuffer->setLabel(state.label);
        core::noteNativeCall(list->core);
        return CommandListImpl::wrap(list);
    }

    //==================================================================================================================
    void CommandList::end()
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        // The core's checks run before any native call.
        core::endList(list.core);
        list.encoders.close(list.core);
        // A barrier still pending orders the work of later lists on the queue.
        metal4::emitTrailing(list.core, list.state, list.encoders.barriers());
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
        // A page's payload is fixed from its creation until the device is destroyed, so it is read without a lock.
        const auto* page = static_cast<const MTL::Buffer*>(list.device->pool<Buffer>().native(place.page));
        const uint64_t value = page->gpuAddress() + place.offset;
        return Addr{ static_cast<uint32_t>(value), static_cast<uint32_t>(value >> 32) };
    }

    //==================================================================================================================
    void CommandList::beginRenderPass([[maybe_unused]] const RenderPassDesc& desc)
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::beginRenderPass");
    }

    //==================================================================================================================
    void CommandList::endRenderPass()
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::endRenderPass");
    }

    //==================================================================================================================
    void CommandList::setPipeline([[maybe_unused]] Pipeline pipeline)
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::setPipeline");
    }

    //==================================================================================================================
    void CommandList::setViewport([[maybe_unused]] const Viewport& viewport)
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::setViewport");
    }

    //==================================================================================================================
    void CommandList::setScissor([[maybe_unused]] const Rect2D& rect)
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::setScissor");
    }

    //==================================================================================================================
    void CommandList::setStencilReference([[maybe_unused]] uint8_t reference)
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::setStencilReference");
    }

    //==================================================================================================================
    void CommandList::setBlendConstants([[maybe_unused]] float r, [[maybe_unused]] float g, [[maybe_unused]] float b, [[maybe_unused]] float a)
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::setBlendConstants");
    }

    //==================================================================================================================
    void CommandList::setPrimitiveTopology([[maybe_unused]] PrimitiveTopology topology)
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::setPrimitiveTopology");
    }

    //==================================================================================================================
    void CommandList::setDepthBias([[maybe_unused]] float constant, [[maybe_unused]] float slopeScaled, [[maybe_unused]] float clamp)
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::setDepthBias");
    }

    //==================================================================================================================
    void CommandList::setRoot([[maybe_unused]] Addr root)
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::setRoot");
    }

    //==================================================================================================================
    void CommandList::draw(
        [[maybe_unused]] uint32_t vertices, [[maybe_unused]] uint32_t instances, [[maybe_unused]] uint32_t firstVertex, [[maybe_unused]] uint32_t firstInstance
    )
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::draw");
    }

    //==================================================================================================================
    void CommandList::drawIndexed(
        [[maybe_unused]] Buffer indices,
        [[maybe_unused]] IndexType type,
        [[maybe_unused]] uint32_t count,
        [[maybe_unused]] uint32_t instances,
        [[maybe_unused]] uint32_t firstIndex,
        [[maybe_unused]] int32_t baseVertex,
        [[maybe_unused]] uint32_t firstInstance
    )
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::drawIndexed");
    }

    //==================================================================================================================
    void CommandList::drawMeshTasks([[maybe_unused]] uint32_t x, [[maybe_unused]] uint32_t y, [[maybe_unused]] uint32_t z)
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::drawMeshTasks");
    }

    //==================================================================================================================
    void CommandList::dispatch([[maybe_unused]] uint32_t x, [[maybe_unused]] uint32_t y, [[maybe_unused]] uint32_t z)
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::dispatch");
    }

    //==================================================================================================================
    void CommandList::drawIndirect([[maybe_unused]] Addr args, [[maybe_unused]] uint32_t maxCount, [[maybe_unused]] Addr count)
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::drawIndirect");
    }

    //==================================================================================================================
    void CommandList::drawIndexedIndirect(
        [[maybe_unused]] Buffer indices,
        [[maybe_unused]] IndexType type,
        [[maybe_unused]] Addr args,
        [[maybe_unused]] uint32_t maxCount,
        [[maybe_unused]] Addr count
    )
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::drawIndexedIndirect");
    }

    //==================================================================================================================
    void CommandList::drawMeshTasksIndirect([[maybe_unused]] Addr args, [[maybe_unused]] uint32_t maxCount, [[maybe_unused]] Addr count)
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::drawMeshTasksIndirect");
    }

    //==================================================================================================================
    void CommandList::dispatchIndirect([[maybe_unused]] Addr args)
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::dispatchIndirect");
    }

    //==================================================================================================================
    void CommandList::clearTexture([[maybe_unused]] Texture texture, [[maybe_unused]] const SubresourceRange& range, [[maybe_unused]] const ClearValue& value)
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::clearTexture");
    }

    //==================================================================================================================
    void CommandList::upscale([[maybe_unused]] Upscaler upscaler, [[maybe_unused]] const UpscaleParams& params)
    {
        CommandListImpl::of(*this);
        metal4::notImplemented("CommandList::upscale");
    }

    //==================================================================================================================
    void CommandList::declareAccessChecked(std::span<const ResourceAccess> accesses)
    {
        // A record for the validation tracker only: no native call.
        core::declareAccess(CommandListImpl::of(*this).core, accesses);
    }
} // namespace rojoRHI
