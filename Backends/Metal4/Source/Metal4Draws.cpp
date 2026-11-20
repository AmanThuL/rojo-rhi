//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Draws.cpp
/// @brief Implements the root recording and the direct draw entries for the Metal 4 backend.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Device.h"

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/Draws.h"
#include "Core/HandlePool.h"
#include "Metal4Buffers.h"
#include "Metal4Common.h"
#include "Metal4DrawState.h"
#include "Metal4Encoders.h"
#include "Metal4PipelineState.h"

#include <Metal/Metal.hpp>

#include <cstdint>

namespace rojoRHI {
    namespace {
        // A direct draw's drawId is 0, and its shaders read the root recorded last. One compare when the table already holds
        // the zero record, one when it holds the root, and one test when the encoder holds the table.
        void bindZeroRecord(CommandListImpl& list)
        {
            list.encoders.bindTable(list.core, list.state);
            const metal4::DrawState& draw = list.draw;
            if (draw.root != draw.tableRoot)
            {
                metal4::applyRoot(list, draw.root);
            }
            if (draw.tableDrawRecord != draw.zeroRecord) [[unlikely]]
            {
                metal4::applyDrawRecord(list, draw.zeroRecord);
            }
        }

        // Out of line, so the common draw, one instance and no pool, stays small enough for link-time optimization to
        // inline.
        [[gnu::noinline]] void sendDraw(CommandListImpl& list, MTL4::RenderCommandEncoder* encoder, uint32_t vertices, uint32_t instances, uint32_t firstVertex)
        {
            // The Metal validation layer autoreleases in a draw it checks, so the draw holds a pool, as a kernel dispatch does.
            const metal4::CommandPool pool(list.state.commandPools);
            if (instances == 1)
            {
                encoder->drawPrimitives(list.draw.topology, firstVertex, vertices);
            }
            else
            {
                encoder->drawPrimitives(list.draw.topology, firstVertex, vertices, instances);
            }
        }

        // The shortest form that says the same with base instance 0. Out of line, as `sendDraw` is.
        [[gnu::noinline]] void sendIndexed(
            CommandListImpl& list,
            MTL4::RenderCommandEncoder* encoder,
            MTL::IndexType type,
            uint32_t count,
            uint64_t address,
            uint64_t length,
            uint32_t instances,
            int32_t baseVertex
        )
        {
            // The validation layer autoreleases in a draw it checks.
            const metal4::CommandPool pool(list.state.commandPools);
            if (baseVertex != 0)
            {
                encoder->drawIndexedPrimitives(list.draw.topology, count, type, address, length, instances, baseVertex, 0);
            }
            else if (instances == 1)
            {
                encoder->drawIndexedPrimitives(list.draw.topology, count, type, address, length);
            }
            else
            {
                encoder->drawIndexedPrimitives(list.draw.topology, count, type, address, length, instances);
            }
        }
    } // namespace

    void CommandList::setRoot(Addr root)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::setRoot(list.core, root);
        const uint64_t address = uint64_t{ root.lo } | uint64_t{ root.hi } << 32;
        // The table takes it at the first draw or dispatch that reads it (`bindZeroRecord`, `applyRoot`), as it takes its bind,
        // so an encoder that only executes writes none. A counted draw's commands bind the root recorded before it
        // themselves, and a counted draw of one record keeps it in its prefix entry.
        list.draw.root = address;
    }

    void CommandList::draw(uint32_t vertices, uint32_t instances, uint32_t firstVertex, [[maybe_unused]] uint32_t firstInstance)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::draw(list.core, vertices, instances);
        if (vertices == 0 || instances == 0)
        {
            return;
        }
        MTL4::RenderCommandEncoder* encoder = metal4::ensureRender(list);
        // rojoRHI::drawId reads 0 from the zero record, after the prefix the open replayed.
        bindZeroRecord(list);
        // Base instance 0, since firstInstance is invisible to shaders. The shortest form that says so is sent: one instance,
        // else the instance count, with base instance 0 either way.
        if (instances == 1 && !list.state.commandPools) [[likely]]
        {
            encoder->drawPrimitives(list.draw.topology, firstVertex, vertices);
        }
        else
        {
            sendDraw(list, encoder, vertices, instances, firstVertex);
        }
        core::noteNativeCall(list.core);
        list.encoders.markWork();
        list.encoders.markRan(MTL::StageVertex | MTL::StageFragment);
    }

    void CommandList::drawIndexed(
        Buffer indices, IndexType type, uint32_t count, uint32_t instances, uint32_t firstIndex, int32_t baseVertex, [[maybe_unused]] uint32_t firstInstance
    )
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::drawIndexed(list.core, indices, type, count, instances, firstIndex);
        if (count == 0 || instances == 0)
        {
            return;
        }
        MTL4::RenderCommandEncoder* encoder = metal4::ensureRender(list);
        bindZeroRecord(list);
        // A record's payload is fixed from creation to retirement, so it is read with one load and no lock (ADR 0041), and it
        // holds the buffer's GPU address and length, so the draw sends the buffer no message.
        const metal4::BufferPayload& buffer = *metal4::bufferPayload(*list.core.device, indices);
        const uint64_t offset = uint64_t{ firstIndex } * (type == IndexType::UInt16 ? 2 : 4);
        const MTL::IndexType native = metal4::indexType(type);
        // One instance and base vertex 0 with no pool is the common draw, sent here in the short form; any other goes out of
        // line.
        if (instances == 1 && baseVertex == 0 && !list.state.commandPools) [[likely]]
        {
            encoder->drawIndexedPrimitives(list.draw.topology, count, native, buffer.address + offset, buffer.length - offset);
        }
        else
        {
            sendIndexed(list, encoder, native, count, buffer.address + offset, buffer.length - offset, instances, baseVertex);
        }
        core::noteNativeCall(list.core);
        list.encoders.markWork();
        list.encoders.markRan(MTL::StageVertex | MTL::StageFragment);
    }

    void CommandList::drawMeshTasks(uint32_t x, uint32_t y, uint32_t z)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::drawMeshTasks(list.core, x, y, z);
        if (x == 0 || y == 0 || z == 0)
        {
            return;
        }
        MTL4::RenderCommandEncoder* encoder = metal4::ensureRender(list);
        // A direct draw's drawId is 0: the task and mesh entries read it from the zero record.
        bindZeroRecord(list);
        const metal4::PipelineState& pipeline = *list.draw.pipeline;
        {
            // The validation layer autoreleases in a draw it checks.
            const metal4::CommandPool pool(list.state.commandPools);
            encoder->drawMeshThreadgroups(MTL::Size(x, y, z), pipeline.objectThreads, pipeline.meshThreads);
        }
        core::noteNativeCall(list.core);
        list.encoders.markWork();
        const bool task = pipeline.objectThreads.width != 0;
        list.encoders.markRan(MTL::StageMesh | MTL::StageFragment | (task ? MTL::StageObject : MTL::Stages{ 0 }));
    }
} // namespace rojoRHI
