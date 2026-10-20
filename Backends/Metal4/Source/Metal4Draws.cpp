//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Draws.cpp
/// @brief Implements the root binding and the direct draw entries for the Metal 4 backend.
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
#include <cstring>

namespace rojoRHI {
    namespace {
        /// The bytes of the zero record a direct mesh draw binds: the leading `uint32` a mesh or task entry reads as `drawId`.
        constexpr uint64_t kZeroRecordBytes = 4;

        /// The alignment the zero record is reserved at.
        constexpr uint32_t kZeroRecordAlignment = 16;

        //==============================================================================================================
        /// Binds the recording's zero record at `buffer(3)` of the list's table unless it holds it, reserving and zeroing
        /// the record in the list's frame-arena page at the recording's first direct mesh draw.
        void bindZeroRecord(CommandListImpl& list)
        {
            metal4::DrawState& draw = list.draw;
            if (draw.zeroRecord == 0)
            {
                // Only a list with no page, or a full one, takes the arena's lock to claim a page (ADR 0020). Arena memory is
                // not cleared, so the record is written here.
                core::DeviceCore& device = *list.core.device;
                const core::ArenaAllocation place =
                    core::bumpArenaPage(device, list.core, kZeroRecordBytes, kZeroRecordAlignment, device.arenaSource(), &device);
                std::memset(place.memory, 0, kZeroRecordBytes);
                draw.zeroRecord = place.address;
            }
            metal4::applyDrawRecord(list, draw.zeroRecord);
        }
    } // namespace

    //==================================================================================================================
    void CommandList::setRoot(Addr root)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::setRoot(list.core, root);
        const uint64_t address = uint64_t{ root.lo } | uint64_t{ root.hi } << 32;
        // A counted mesh draw's commands bind the root recorded before the draw themselves.
        list.draw.root = address;
        if (list.draw.passOpen && list.encoders.renderEncoder() == nullptr)
        {
            // Inside a pass whose encoder is closed the root waits in the prefix, so a command replayed there sees the root
            // recorded before it.
            metal4::PrefixCommand command{ .kind = metal4::PrefixKind::Root };
            command.root = address;
            list.draw.record(command);
            return;
        }
        metal4::applyRoot(list, address);
    }

    //==================================================================================================================
    void CommandList::draw(uint32_t vertices, uint32_t instances, uint32_t firstVertex, [[maybe_unused]] uint32_t firstInstance)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::draw(list.core, vertices, instances);
        if (vertices == 0 || instances == 0)
        {
            return;
        }
        MTL4::RenderCommandEncoder* encoder = metal4::ensureRender(list);
        {
            // The Metal validation layer autoreleases in a draw it checks, so the draw holds a pool, as a kernel dispatch does.
            const metal4::AutoreleasePool pool;
            // Base instance 0: rojoRHI::drawId reads it in the vertex stage, and firstInstance is invisible to shaders.
            encoder->drawPrimitives(list.draw.topology, firstVertex, vertices, instances, 0);
        }
        core::noteNativeCall(list.core);
        list.encoders.markWork();
        list.encoders.markRan(MTL::StageVertex | MTL::StageFragment);
    }

    //==================================================================================================================
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
        // A record's payload is fixed from creation to retirement, so it is read with one load and no lock (ADR 0020), and it
        // holds the buffer's GPU address and length, so the draw sends the buffer no message.
        const metal4::BufferPayload& buffer = *metal4::bufferPayload(*list.core.device, indices);
        const uint64_t offset = uint64_t{ firstIndex } * (type == IndexType::UInt16 ? 2 : 4);
        {
            // The validation layer autoreleases in a draw it checks.
            const metal4::AutoreleasePool pool;
            encoder->drawIndexedPrimitives(
                list.draw.topology, count, metal4::indexType(type), buffer.address + offset, buffer.length - offset, instances, baseVertex, 0
            );
        }
        core::noteNativeCall(list.core);
        list.encoders.markWork();
        list.encoders.markRan(MTL::StageVertex | MTL::StageFragment);
    }

    //==================================================================================================================
    void CommandList::drawMeshTasks(uint32_t x, uint32_t y, uint32_t z)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::drawMeshTasks(list.core, x, y, z);
        if (x == 0 || y == 0 || z == 0)
        {
            return;
        }
        MTL4::RenderCommandEncoder* encoder = metal4::ensureRender(list);
        // A direct draw's drawId is 0: the task and mesh entries read it from a zero record.
        bindZeroRecord(list);
        const metal4::PipelineState& pipeline = *list.draw.pipeline;
        {
            // The validation layer autoreleases in a draw it checks.
            const metal4::AutoreleasePool pool;
            encoder->drawMeshThreadgroups(MTL::Size(x, y, z), pipeline.objectThreads, pipeline.meshThreads);
        }
        core::noteNativeCall(list.core);
        list.encoders.markWork();
        const bool task = pipeline.objectThreads.width != 0;
        list.encoders.markRan(MTL::StageMesh | MTL::StageFragment | (task ? MTL::StageObject : MTL::Stages{ 0 }));
    }
} // namespace rojoRHI
