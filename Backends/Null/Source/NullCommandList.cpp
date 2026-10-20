//----------------------------------------------------------------------------------------------------------------------
/// @file NullCommandList.cpp
/// @brief Implements the public command list entries for the Null backend: lists keep a state, frame data lives in host buffers.
//----------------------------------------------------------------------------------------------------------------------

#include "NullDevice.h"

#include <rojoRHI/RHI.h>

#include "Base/Log.h"
#include "Core/Accesses.h"
#include "Core/Barriers.h"
#include "Core/Buffers.h"
#include "Core/CommandLists.h"
#include "Core/Copies.h"
#include "Core/DeviceCore.h"
#include "Core/Draws.h"
#include "Core/FrameArena.h"
#include "Core/RenderPasses.h"
#include "Core/Upscalers.h"

#include <cstdlib>
#include <cstring>
#include <format>
#include <utility>

namespace rojoRHI {
    //==================================================================================================================
    Result<core::ArenaPage> createArenaPage(void* context, uint64_t size)
    {
        core::DeviceCore& core = *static_cast<core::DeviceCore*>(context);
        const BufferDesc desc{ .size = size, .memory = Memory::Upload, .usage = BufferUsage::IndirectArgs, .label = "rojorhi.frameArena.page" };
        core::BufferRecord record = core::prepareBuffer(core, desc, {}, {}, {});
        auto* host = static_cast<std::byte*>(std::calloc(size, 1));
        if (host == nullptr)
        {
            return std::unexpected(Error{ ErrorCategory::OutOfMemory, std::format("no host memory for {} bytes of a frame arena page", size) });
        }
        record.native = host;
        Result<Buffer> page = core::addFrameArenaPage(core, std::move(record));
        if (!page)
        {
            std::free(host);
            return std::unexpected(std::move(page.error()));
        }
        core::DeviceCounts::add(core.counts().cpuAllocations);
        // Null addresses encode a slot index and an offset, not a GPU address, so the page has none.
        return core::ArenaPage{ .buffer = *page, .host = host, .size = size, .address = 0 };
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
                core::DeviceCounts::add(impl.core.counts().cpuAllocations);
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

    //==================================================================================================================
    void CommandList::beginRenderPass(const RenderPassDesc& desc)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::beginPass(list, desc);
        core::noteNativeCall(list);
        ++list.counts.encodersOpened;
    }

    //==================================================================================================================
    void CommandList::endRenderPass()
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::endPass(list);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::beginScope(std::string_view label)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::beginScope(list, label);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::endScope()
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::endScope(list);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::setPipeline(Pipeline pipeline)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::bindPipeline(list, pipeline);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::setViewport(const Viewport& viewport)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::setViewport(list, viewport);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::setScissor(const Rect2D& rect)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::setScissor(list, rect);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::setStencilReference([[maybe_unused]] uint8_t reference)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::setStencilReference(list);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::setBlendConstants([[maybe_unused]] float r, [[maybe_unused]] float g, [[maybe_unused]] float b, [[maybe_unused]] float a)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::setBlendConstants(list);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::setPrimitiveTopology(PrimitiveTopology topology)
    {
        core::setPrimitiveTopology(CommandListImpl::of(*this).core, topology);
    }

    //==================================================================================================================
    void CommandList::setDepthBias([[maybe_unused]] float constant, [[maybe_unused]] float slopeScaled, [[maybe_unused]] float clamp)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::setDepthBias(list);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::setRoot(Addr root)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::setRoot(list, root);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::draw(uint32_t vertices, uint32_t instances, [[maybe_unused]] uint32_t firstVertex, [[maybe_unused]] uint32_t firstInstance)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::draw(list, vertices, instances);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::drawIndexed(
        Buffer indices,
        IndexType type,
        uint32_t count,
        uint32_t instances,
        uint32_t firstIndex,
        [[maybe_unused]] int32_t baseVertex,
        [[maybe_unused]] uint32_t firstInstance
    )
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::drawIndexed(list, indices, type, count, instances, firstIndex);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::drawMeshTasks(uint32_t x, uint32_t y, uint32_t z)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::drawMeshTasks(list, x, y, z);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::dispatch(uint32_t x, uint32_t y, uint32_t z)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::dispatch(list, x, y, z);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::drawIndirect(Addr args, uint32_t maxCount, Addr count)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::drawIndirect(list, args, maxCount, count);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::drawIndexedIndirect(Buffer indices, [[maybe_unused]] IndexType type, Addr args, uint32_t maxCount, Addr count)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::drawIndexedIndirect(list, indices, args, maxCount, count);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::drawMeshTasksIndirect(Addr args, uint32_t maxCount, Addr count)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::drawMeshTasksIndirect(list, args, maxCount, count);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::dispatchIndirect(Addr args)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::dispatchIndirect(list, args);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::copyBuffer(Buffer src, uint64_t srcOffset, Buffer dst, uint64_t dstOffset, uint64_t size)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::copyBuffer(list, src, srcOffset, dst, dstOffset, size);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::copyBufferToTexture(Buffer src, const BufferTextureLayout& layout, Texture dst, const TextureRegion& region)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::copyBufferToTexture(list, src, layout, dst, region);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::copyTextureToBuffer(Texture src, const TextureRegion& region, Buffer dst, const BufferTextureLayout& layout)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::copyTextureToBuffer(list, src, region, dst, layout);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::copyTexture(Texture src, const TextureRegion& srcRegion, Texture dst, const TextureRegion& dstRegion)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::copyTexture(list, src, srcRegion, dst, dstRegion);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::fillBuffer(Buffer dst, uint64_t offset, uint64_t size, [[maybe_unused]] uint32_t pattern)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::fillBuffer(list, dst, offset, size);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::clearTexture(Texture texture, const SubresourceRange& range, const ClearValue& value)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::clearTexture(list, texture, range, value);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::upscale(Upscaler upscaler, const UpscaleParams& params)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::upscale(list, upscaler, params);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void CommandList::declareAccessChecked(std::span<const ResourceAccess> accesses)
    {
        core::declareAccess(CommandListImpl::of(*this).core, accesses);
    }

    //==================================================================================================================
    void CommandList::barrier(std::span<const GlobalBarrier> globals, std::span<const BufferBarrier> buffers, std::span<const TextureBarrier> textures)
    {
        core::CommandListCore& list = CommandListImpl::of(*this).core;
        core::barrier(list, globals, buffers, textures);
        if (!globals.empty() || !buffers.empty() || !textures.empty())
        {
            ++list.counts.nativeBarriers;
            core::noteNativeCall(list);
        }
    }
} // namespace rojoRHI
