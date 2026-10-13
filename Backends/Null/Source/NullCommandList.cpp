//----------------------------------------------------------------------------------------------------------------------
/// @file NullCommandList.cpp
/// @brief Implements the public command list entries for the Null backend: lists keep a state, frame data lives in host buffers.
//----------------------------------------------------------------------------------------------------------------------

#include "NullDevice.h"

#include <rojoRHI/CommandList.h>
#include <rojoRHI/Device.h>

#include "Base/Log.h"
#include "Core/Barriers.h"
#include "Core/Buffers.h"
#include "Core/CommandLists.h"
#include "Core/Copies.h"
#include "Core/DeviceCore.h"
#include "Core/Draws.h"
#include "Core/FrameArena.h"
#include "Core/RenderPasses.h"

#include <cstdlib>
#include <cstring>
#include <format>
#include <utility>

namespace rojoRHI {
    namespace {
        //==============================================================================================================
        /// Creates one arena page as a zeroed host buffer of the pool, so an address into it resolves like any buffer's. The
        /// page has the `IndirectArgs` usage because the reference lets frame data serve as indirect arguments.
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

    //==================================================================================================================
    void CommandList::beginRenderPass(const RenderPassDesc& desc)
    {
        core::beginPass(CommandListImpl::of(*this).core, desc);
    }

    //==================================================================================================================
    void CommandList::endRenderPass()
    {
        core::endPass(CommandListImpl::of(*this).core);
    }

    //==================================================================================================================
    void CommandList::beginScope([[maybe_unused]] std::string_view label)
    {
        core::beginScope(CommandListImpl::of(*this).core);
    }

    //==================================================================================================================
    void CommandList::endScope()
    {
        core::endScope(CommandListImpl::of(*this).core);
    }

    //==================================================================================================================
    void CommandList::setPipeline(Pipeline pipeline)
    {
        core::bindPipeline(CommandListImpl::of(*this).core, pipeline);
    }

    //==================================================================================================================
    void CommandList::setViewport(const Viewport& viewport)
    {
        core::setViewport(CommandListImpl::of(*this).core, viewport);
    }

    //==================================================================================================================
    void CommandList::setScissor(const Rect2D& rect)
    {
        core::setScissor(CommandListImpl::of(*this).core, rect);
    }

    //==================================================================================================================
    void CommandList::setStencilReference([[maybe_unused]] uint8_t reference)
    {
        core::setStencilReference(CommandListImpl::of(*this).core);
    }

    //==================================================================================================================
    void CommandList::setBlendConstants([[maybe_unused]] float r, [[maybe_unused]] float g, [[maybe_unused]] float b, [[maybe_unused]] float a)
    {
        core::setBlendConstants(CommandListImpl::of(*this).core);
    }

    //==================================================================================================================
    void CommandList::setPrimitiveTopology(PrimitiveTopology topology)
    {
        core::setPrimitiveTopology(CommandListImpl::of(*this).core, topology);
    }

    //==================================================================================================================
    void CommandList::setDepthBias([[maybe_unused]] float constant, [[maybe_unused]] float slopeScaled, [[maybe_unused]] float clamp)
    {
        core::setDepthBias(CommandListImpl::of(*this).core);
    }

    //==================================================================================================================
    void CommandList::setRoot(Addr root)
    {
        core::setRoot(CommandListImpl::of(*this).core, root);
    }

    //==================================================================================================================
    void CommandList::draw(
        [[maybe_unused]] uint32_t vertices, [[maybe_unused]] uint32_t instances, [[maybe_unused]] uint32_t firstVertex, [[maybe_unused]] uint32_t firstInstance
    )
    {
        core::draw(CommandListImpl::of(*this).core);
    }

    //==================================================================================================================
    void CommandList::drawIndexed(
        Buffer indices,
        IndexType type,
        uint32_t count,
        [[maybe_unused]] uint32_t instances,
        uint32_t firstIndex,
        [[maybe_unused]] int32_t baseVertex,
        [[maybe_unused]] uint32_t firstInstance
    )
    {
        core::drawIndexed(CommandListImpl::of(*this).core, indices, type, count, firstIndex);
    }

    //==================================================================================================================
    void CommandList::drawMeshTasks(uint32_t x, uint32_t y, uint32_t z)
    {
        core::drawMeshTasks(CommandListImpl::of(*this).core, x, y, z);
    }

    //==================================================================================================================
    void CommandList::dispatch(uint32_t x, uint32_t y, uint32_t z)
    {
        core::dispatch(CommandListImpl::of(*this).core, x, y, z);
    }

    //==================================================================================================================
    void CommandList::drawIndirect(Addr args, uint32_t maxCount, Addr count)
    {
        core::drawIndirect(CommandListImpl::of(*this).core, args, maxCount, count);
    }

    //==================================================================================================================
    void CommandList::drawIndexedIndirect(Buffer indices, [[maybe_unused]] IndexType type, Addr args, uint32_t maxCount, Addr count)
    {
        core::drawIndexedIndirect(CommandListImpl::of(*this).core, indices, args, maxCount, count);
    }

    //==================================================================================================================
    void CommandList::drawMeshTasksIndirect(Addr args, uint32_t maxCount, Addr count)
    {
        core::drawMeshTasksIndirect(CommandListImpl::of(*this).core, args, maxCount, count);
    }

    //==================================================================================================================
    void CommandList::dispatchIndirect(Addr args)
    {
        core::dispatchIndirect(CommandListImpl::of(*this).core, args);
    }

    //==================================================================================================================
    void CommandList::copyBuffer(Buffer src, uint64_t srcOffset, Buffer dst, uint64_t dstOffset, uint64_t size)
    {
        core::copyBuffer(CommandListImpl::of(*this).core, src, srcOffset, dst, dstOffset, size);
    }

    //==================================================================================================================
    void CommandList::copyBufferToTexture(Buffer src, const BufferTextureLayout& layout, Texture dst, const TextureRegion& region)
    {
        core::copyBufferToTexture(CommandListImpl::of(*this).core, src, layout, dst, region);
    }

    //==================================================================================================================
    void CommandList::copyTextureToBuffer(Texture src, const TextureRegion& region, Buffer dst, const BufferTextureLayout& layout)
    {
        core::copyTextureToBuffer(CommandListImpl::of(*this).core, src, region, dst, layout);
    }

    //==================================================================================================================
    void CommandList::copyTexture(Texture src, const TextureRegion& srcRegion, Texture dst, const TextureRegion& dstRegion)
    {
        core::copyTexture(CommandListImpl::of(*this).core, src, srcRegion, dst, dstRegion);
    }

    //==================================================================================================================
    void CommandList::fillBuffer(Buffer dst, uint64_t offset, uint64_t size, [[maybe_unused]] uint32_t pattern)
    {
        core::fillBuffer(CommandListImpl::of(*this).core, dst, offset, size);
    }

    //==================================================================================================================
    void CommandList::clearTexture(Texture texture, const SubresourceRange& range, const ClearValue& value)
    {
        core::clearTexture(CommandListImpl::of(*this).core, texture, range, value);
    }

    //==================================================================================================================
    void CommandList::barrier(std::span<const GlobalBarrier> globals, std::span<const BufferBarrier> buffers, std::span<const TextureBarrier> textures)
    {
        core::barrier(CommandListImpl::of(*this).core, globals, buffers, textures);
    }
} // namespace rojoRHI
