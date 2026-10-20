//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12CommandList.cpp
/// @brief Implements D3D12 command list acquisition, `end` and frame data on the core's state; other recording entries abort.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Device.h"

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/FrameArena.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "D3D12Addr.h"
#include "D3D12Buffers.h"
#include "D3D12Common.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace rojoRHI {
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
        // A list is the core's state alone: it encodes no native command, so acquisition makes no native call.
        const uint32_t slot = static_cast<uint32_t>(impl.core.frameNumber() % impl.core.framesInFlight());
        CommandListImpl& list = impl.lists.acquire(slot, type, impl.core.counts());
        core::beginList(list.core, impl.core, type, label);
        return CommandListImpl::wrap(&list);
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
        // Only a list with no page, or a full one, takes the arena's lock to claim a page (ADR 0020).
        const core::ArenaAllocation place = core::allocateFrameData(*list.device, list, bytes, alignment, d3d12::createArenaPage, list.device);
        std::memcpy(place.memory, bytes.data(), bytes.size());
        // The page's payload is fixed while the page lives, so it is read with one acquire load and no lock.
        const auto* page = static_cast<const d3d12::BufferPayload*>(list.device->pool<Buffer>().native(place.page));
        return d3d12::encodeAddr(*page, place.offset);
    }

    //==================================================================================================================
    void CommandList::beginRenderPass(const RenderPassDesc&)
    {
        d3d12::notImplemented("CommandList::beginRenderPass");
    }

    //==================================================================================================================
    void CommandList::endRenderPass()
    {
        d3d12::notImplemented("CommandList::endRenderPass");
    }

    //==================================================================================================================
    void CommandList::beginScope(std::string_view)
    {
        d3d12::notImplemented("CommandList::beginScope");
    }

    //==================================================================================================================
    void CommandList::endScope()
    {
        d3d12::notImplemented("CommandList::endScope");
    }

    //==================================================================================================================
    void CommandList::setPipeline(Pipeline)
    {
        d3d12::notImplemented("CommandList::setPipeline");
    }

    //==================================================================================================================
    void CommandList::setViewport(const Viewport&)
    {
        d3d12::notImplemented("CommandList::setViewport");
    }

    //==================================================================================================================
    void CommandList::setScissor(const Rect2D&)
    {
        d3d12::notImplemented("CommandList::setScissor");
    }

    //==================================================================================================================
    void CommandList::setStencilReference(uint8_t)
    {
        d3d12::notImplemented("CommandList::setStencilReference");
    }

    //==================================================================================================================
    void CommandList::setBlendConstants(float, float, float, float)
    {
        d3d12::notImplemented("CommandList::setBlendConstants");
    }

    //==================================================================================================================
    void CommandList::setPrimitiveTopology(PrimitiveTopology)
    {
        d3d12::notImplemented("CommandList::setPrimitiveTopology");
    }

    //==================================================================================================================
    void CommandList::setDepthBias(float, float, float)
    {
        d3d12::notImplemented("CommandList::setDepthBias");
    }

    //==================================================================================================================
    void CommandList::setRoot(Addr)
    {
        d3d12::notImplemented("CommandList::setRoot");
    }

    //==================================================================================================================
    void CommandList::draw(uint32_t, uint32_t, uint32_t, uint32_t)
    {
        d3d12::notImplemented("CommandList::draw");
    }

    //==================================================================================================================
    void CommandList::drawIndexed(Buffer, IndexType, uint32_t, uint32_t, uint32_t, int32_t, uint32_t)
    {
        d3d12::notImplemented("CommandList::drawIndexed");
    }

    //==================================================================================================================
    void CommandList::drawMeshTasks(uint32_t, uint32_t, uint32_t)
    {
        d3d12::notImplemented("CommandList::drawMeshTasks");
    }

    //==================================================================================================================
    void CommandList::dispatch(uint32_t, uint32_t, uint32_t)
    {
        d3d12::notImplemented("CommandList::dispatch");
    }

    //==================================================================================================================
    void CommandList::drawIndirect(Addr, uint32_t, Addr)
    {
        d3d12::notImplemented("CommandList::drawIndirect");
    }

    //==================================================================================================================
    void CommandList::drawIndexedIndirect(Buffer, IndexType, Addr, uint32_t, Addr)
    {
        d3d12::notImplemented("CommandList::drawIndexedIndirect");
    }

    //==================================================================================================================
    void CommandList::drawMeshTasksIndirect(Addr, uint32_t, Addr)
    {
        d3d12::notImplemented("CommandList::drawMeshTasksIndirect");
    }

    //==================================================================================================================
    void CommandList::dispatchIndirect(Addr)
    {
        d3d12::notImplemented("CommandList::dispatchIndirect");
    }

    //==================================================================================================================
    void CommandList::copyBuffer(Buffer, uint64_t, Buffer, uint64_t, uint64_t)
    {
        d3d12::notImplemented("CommandList::copyBuffer");
    }

    //==================================================================================================================
    void CommandList::copyBufferToTexture(Buffer, const BufferTextureLayout&, Texture, const TextureRegion&)
    {
        d3d12::notImplemented("CommandList::copyBufferToTexture");
    }

    //==================================================================================================================
    void CommandList::copyTextureToBuffer(Texture, const TextureRegion&, Buffer, const BufferTextureLayout&)
    {
        d3d12::notImplemented("CommandList::copyTextureToBuffer");
    }

    //==================================================================================================================
    void CommandList::copyTexture(Texture, const TextureRegion&, Texture, const TextureRegion&)
    {
        d3d12::notImplemented("CommandList::copyTexture");
    }

    //==================================================================================================================
    void CommandList::fillBuffer(Buffer, uint64_t, uint64_t, uint32_t)
    {
        d3d12::notImplemented("CommandList::fillBuffer");
    }

    //==================================================================================================================
    void CommandList::clearTexture(Texture, const SubresourceRange&, const ClearValue&)
    {
        d3d12::notImplemented("CommandList::clearTexture");
    }

    //==================================================================================================================
    void CommandList::barrier(std::span<const GlobalBarrier>, std::span<const BufferBarrier>, std::span<const TextureBarrier>)
    {
        d3d12::notImplemented("CommandList::barrier");
    }

    //==================================================================================================================
    void CommandList::upscale(Upscaler, const UpscaleParams&)
    {
        d3d12::notImplemented("CommandList::upscale");
    }

    //==================================================================================================================
    void CommandList::declareAccessChecked(std::span<const ResourceAccess>)
    {
        d3d12::notImplemented("CommandList::declareAccess");
    }
} // namespace rojoRHI
