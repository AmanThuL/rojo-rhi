//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Indirect.cpp
/// @brief Implements the indirect draw entries for the Metal 4 backend: a kernel ahead of the render encoder encodes or writes each counted draw.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Device.h"

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/Draws.h"
#include "Core/HandlePool.h"
#include "Metal4Barriers.h"
#include "Metal4Common.h"
#include "Metal4DrawState.h"
#include "Metal4Encoders.h"
#include "Metal4Icb.h"
#include "Metal4Kernels.h"
#include "Metal4PipelineState.h"

#include <Metal/Metal.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace rojoRHI {
    namespace {
        /// The bytes a counted draw reserves in the list's frame-arena page: its kernel's `DrawParams`, then either the
        /// indirect command buffer's resource ID and the execution range, or the native record the writer kernel writes.
        constexpr uint64_t kReservationBytes = 96;

        /// The alignment of the reservation.
        constexpr uint32_t kReservationAlignment = 16;

        /// Where the indirect command buffer's resource ID lies in the reservation.
        constexpr uint64_t kIcbIdOffset = sizeof(metal4::DrawParams);

        /// Where the execution range lies in the reservation.
        constexpr uint64_t kRangeOffset = kIcbIdOffset + 16;

        /// Where the writer kernel's native record lies in the reservation.
        constexpr uint64_t kRecordOffset = sizeof(metal4::DrawParams);

        static_assert(kRangeOffset + sizeof(MTL::IndirectCommandBufferExecutionRange) <= kReservationBytes);
        static_assert(kRecordOffset + sizeof(MTL::DrawIndexedPrimitivesIndirectArguments) <= kReservationBytes);

        /// The bytes a counted mesh draw reserves: its kernel's `MeshParams`, then either the indirect command buffer's
        /// resource ID and the execution range, or the native record the writer kernel writes.
        constexpr uint64_t kMeshReservationBytes = 128;

        static_assert(metal4::kMeshRangeOffset + sizeof(MTL::IndirectCommandBufferExecutionRange) <= kMeshReservationBytes);
        static_assert(metal4::kMeshOutputOffset + sizeof(MTL::DispatchThreadgroupsIndirectArguments) <= kMeshReservationBytes);

        /// The stages that read what a counted draw's kernel wrote: Metal reads the indirect command buffer, the range and a
        /// native record before vertex shading.
        constexpr MTL::Stages kDrawReads = MTL::StageVertex;

        /// The stages that read what a counted mesh draw's kernel wrote: before object shading, or mesh shading without a task
        /// entry.
        constexpr MTL::Stages kMeshReads = MTL::StageObject | MTL::StageMesh;

        /// One counted draw as the entries pass it on.
        struct Counted
        {
            metal4::IcbKind kind;     ///< Indexed or not.
            uint64_t records;         ///< The GPU address of the first record.
            uint32_t maxCount;        ///< The most records drawn.
            uint64_t count;           ///< The GPU address of the count, or 0 for none.
            Buffer indices;           ///< The index buffer of an indexed draw; null otherwise.
            MTL::IndexType indexType; ///< The index type of an indexed draw.
        };

        /// Bytes a counted draw reserved in its list's frame-arena page.
        struct Reservation
        {
            std::byte* memory; ///< Their host address.
            uint64_t address;  ///< Their GPU address.
        };

        //==============================================================================================================
        /// The GPU address `address` names.
        uint64_t addressOf(Addr address)
        {
            return uint64_t{ address.lo } | uint64_t{ address.hi } << 32;
        }

        //==============================================================================================================
        /// Reserves `bytes` for a counted draw in `list`'s frame-arena page. Only a list with no page, or a full one, takes the
        /// arena's lock to claim a page (ADR 0020). These bytes are no frame data, so they count as no upload.
        Reservation reserve(CommandListImpl& list, uint64_t bytes)
        {
            core::DeviceCore& device = *list.core.device;
            metal4::DrawState& draw = list.draw;
            const core::ArenaAllocation place = core::bumpArenaPage(device, list.core, bytes, kReservationAlignment, device.arenaSource(), &device);
            if (place.page != draw.arenaPage)
            {
                // A page's payload is fixed from its creation until the device is destroyed, so it is read without a lock.
                draw.arenaPage = place.page;
                draw.arenaPageAddress = static_cast<const MTL::Buffer*>(device.pool<Buffer>().native(place.page))->gpuAddress();
            }
            return Reservation{ place.memory, draw.arenaPageAddress + place.offset };
        }

        //==============================================================================================================
        /// Runs a counted draw's `kernel` over the parameters at `params` in `groups` threadgroups of `threads` threads, on the
        /// compute encoder ahead of the render encoder: the kernels of a pass's leading counted draws share it, and it binds the
        /// kernel table once. A barrier the caller recorded for the records and the count reaches the dispatch stage when the
        /// encoder opens. A backend barrier from the dispatch stage to `reads`, where the execute reads what the kernel wrote,
        /// joins the pending barrier, which the render encoder emits when it opens; after a split (`split`) it also orders the
        /// ended encoder's fragment-stage stores before the resumed encoder's loads.
        void runKernel(
            CommandListImpl& list, const MTL::ComputePipelineState* kernel, uint64_t params, MTL::Size groups, MTL::Size threads, MTL::Stages reads, bool split
        )
        {
            // The encoder runs only kernels, which bind the kernel table, so it binds no list table.
            MTL4::ComputeCommandEncoder* encoder = list.encoders.compute(list.core, list.state, MTL::StageDispatch, true);
            metal4::bindKernelBatch(list.core, list.state, list.encoders, kernel);
            list.state.kernelTable->setAddress(params, 0);
            core::noteNativeCall(list.core);
            metal4::dispatchKernelBatch(list.core, encoder, groups, threads);
            list.encoders.markWork();
            list.encoders.barriers().merge(MTL::StageDispatch, reads, MTL4::VisibilityOptionDevice);
            if (split)
            {
                // Metal 4 tracks no hazard between render encoders: the ended encoder's stores must finish before the resumed
                // encoder loads, which the barrier the resumed encoder emits at its open orders at no further call.
                list.encoders.barriers().merge(MTL::StageFragment, MTL::StageFragment, MTL4::VisibilityOptionDevice);
            }
        }

        //==============================================================================================================
        /// The threadgroups of an encode kernel over `maxCount` records, one thread each.
        MTL::Size encodeGroups(uint32_t maxCount)
        {
            return MTL::Size((maxCount + metal4::kDrawGroupThreads - 1) / metal4::kDrawGroupThreads, 1, 1);
        }

        //==============================================================================================================
        /// Records `counted` (chapter 08's Metal 4 mapping). Its kernel runs on a compute encoder ahead of the render encoder that
        /// executes it, so an open render encoder splits the pass first; the kernels of a pass's leading counted draws share one
        /// compute encoder, which binds the kernel table once. Above `maxCount` 1 the kernel encodes the records below the count
        /// into an indirect command buffer of the list and writes its range; at 1 it writes the record as a native one. A
        /// backend barrier from the dispatch stage to the vertex stage, where the execute reads what the kernel wrote, joins the
        /// pending barrier, which the render encoder emits when it opens; after a split it also orders the ended encoder's
        /// fragment-stage stores before the resumed encoder's loads. The execute joins the prefix, after the state recorded
        /// before it.
        void recordCounted(CommandListImpl& list, const Counted& counted)
        {
            const bool split = list.encoders.renderEncoder() != nullptr;
            metal4::splitPass(list);
            core::DeviceCore& device = *list.core.device;
            metal4::DrawState& draw = list.draw;
            // The retire hooks' context is the device that owns the core; its kernels and native device are fixed from its
            // creation to its destruction.
            const DeviceImpl& impl = *static_cast<const DeviceImpl*>(device.retireHooks().context);
            const Reservation place = reserve(list, kReservationBytes);
            const uint64_t base = place.address;
            const bool indexed = counted.kind == metal4::IcbKind::Indexed;
            if (indexed && counted.indices != draw.indexBuffer)
            {
                // A record's payload is fixed from creation to retirement, so the buffer is read with one load and no lock (ADR 0020).
                const auto* buffer = static_cast<const MTL::Buffer*>(device.pool<Buffer>().native(counted.indices));
                draw.indexBuffer = counted.indices;
                draw.indexAddress = buffer->gpuAddress();
                draw.indexLength = buffer->length();
            }
            metal4::DrawParams params{ .icb = 0,
                                       .records = counted.records,
                                       .count = counted.count,
                                       .output = 0,
                                       .indices = indexed ? draw.indexAddress : 0,
                                       .maxCount = counted.maxCount,
                                       .primitive = static_cast<uint32_t>(draw.topology),
                                       .indexType = static_cast<uint32_t>(counted.indexType),
                                       .reserved = {} };
            metal4::PrefixCommand command{};
            const MTL::ComputePipelineState* kernel = nullptr;
            MTL::Size groups(1, 1, 1);
            MTL::Size threads(1, 1, 1);
            if (counted.maxCount > 1)
            {
                const metal4::IcbSlot& icb = draw.icbs.take(impl.device, device, counted.kind, counted.maxCount);
                draw.icbs.attach(list.core, list.state.commandBuffer);
                std::memcpy(place.memory + kIcbIdOffset, &icb.id, sizeof(icb.id));
                params.icb = base + kIcbIdOffset;
                params.output = base + kRangeOffset;
                kernel = indexed ? impl.kernels.encodeIndexed : impl.kernels.encodeDraw;
                groups = encodeGroups(counted.maxCount);
                threads = MTL::Size(metal4::kDrawGroupThreads, 1, 1);
                command.kind = metal4::PrefixKind::Execute;
                command.execute = metal4::PrefixExecute{ .buffer = icb.buffer, .range = base + kRangeOffset };
            }
            else
            {
                params.output = base + kRecordOffset;
                kernel = indexed ? impl.kernels.writeIndexed : impl.kernels.writeDraw;
                command.kind = indexed ? metal4::PrefixKind::IndexedIndirect : metal4::PrefixKind::DrawIndirect;
                command.indirect = metal4::PrefixIndirect{ .record = base + kRecordOffset,
                                                           .primitive = draw.topology,
                                                           .indexType = counted.indexType,
                                                           .indices = params.indices,
                                                           .indexLength = indexed ? draw.indexLength : 0 };
            }
            std::memcpy(place.memory, &params, sizeof(params));
            runKernel(list, kernel, base, groups, threads, kDrawReads, split);
            // The execute observes the state recorded before it, so no later entry merges across it.
            draw.appendBarrierEntry(command);
        }

        //==============================================================================================================
        /// Records a counted mesh draw of `maxCount` records from `records`, with the count at `count` or none for 0, as
        /// `recordCounted` records the others. Above `maxCount` 1 the kernel encodes a mesh indirect command buffer whose
        /// commands bind the reserved buffers themselves, with the root recorded before this call: the prefix keeps calls in
        /// order, so that root is the one the table holds at the execute. At 1 the kernel writes the record as a native one,
        /// and the draw binds the caller's record at `buffer(3)`. The bound pipeline gives the threadgroup sizes.
        void recordCountedMesh(CommandListImpl& list, uint64_t records, uint32_t maxCount, uint64_t count)
        {
            const bool split = list.encoders.renderEncoder() != nullptr;
            metal4::splitPass(list);
            core::DeviceCore& device = *list.core.device;
            metal4::DrawState& draw = list.draw;
            // The retire hooks' context is the device that owns the core; its kernels, native device and tables are fixed from
            // its creation to its destruction.
            const DeviceImpl& impl = *static_cast<const DeviceImpl*>(device.retireHooks().context);
            if (draw.viewTableAddress == 0)
            {
                draw.viewTableAddress = impl.viewTable->gpuAddress();
                draw.samplerTableAddress = impl.samplerTable->gpuAddress();
            }
            const metal4::PipelineState& pipeline = *draw.pipeline;
            const Reservation place = reserve(list, kMeshReservationBytes);
            const uint64_t base = place.address;
            metal4::MeshParams params{ .icb = 0,
                                       .records = records,
                                       .count = count,
                                       .output = base + metal4::kMeshOutputOffset,
                                       .root = draw.root,
                                       .views = draw.viewTableAddress,
                                       .samplers = draw.samplerTableAddress,
                                       .maxCount = maxCount,
                                       .objectThreads = { static_cast<uint32_t>(pipeline.objectThreads.width),
                                                          static_cast<uint32_t>(pipeline.objectThreads.height),
                                                          static_cast<uint32_t>(pipeline.objectThreads.depth) },
                                       .meshThreads = { static_cast<uint32_t>(pipeline.meshThreads.width),
                                                        static_cast<uint32_t>(pipeline.meshThreads.height),
                                                        static_cast<uint32_t>(pipeline.meshThreads.depth) },
                                       .reserved = {} };
            metal4::PrefixCommand command{};
            const MTL::ComputePipelineState* kernel = impl.kernels.writeMesh;
            MTL::Size groups(1, 1, 1);
            MTL::Size threads(1, 1, 1);
            if (maxCount > 1)
            {
                const metal4::IcbSlot& icb = draw.icbs.take(impl.device, device, metal4::IcbKind::Mesh, maxCount);
                draw.icbs.attach(list.core, list.state.commandBuffer);
                std::memcpy(place.memory + metal4::kMeshOutputOffset, &icb.id, sizeof(icb.id));
                params.icb = base + metal4::kMeshOutputOffset;
                params.output = base + metal4::kMeshRangeOffset;
                kernel = impl.kernels.encodeMesh;
                groups = encodeGroups(maxCount);
                threads = MTL::Size(metal4::kDrawGroupThreads, 1, 1);
                command.kind = metal4::PrefixKind::MeshExecute;
                command.execute = metal4::PrefixExecute{ .buffer = icb.buffer, .range = base + metal4::kMeshRangeOffset };
            }
            else
            {
                command.kind = metal4::PrefixKind::MeshIndirect;
                command.mesh = metal4::PrefixMesh{ .record = records, .native = base + metal4::kMeshOutputOffset, .pipeline = &pipeline };
            }
            std::memcpy(place.memory, &params, sizeof(params));
            runKernel(list, kernel, base, groups, threads, kMeshReads, split);
            // The execute observes the state recorded before it, so no later entry merges across it.
            draw.appendBarrierEntry(command);
        }

        //==============================================================================================================
        /// Records a lone mesh record at `record` with no count, which needs no kernel: on the render encoder, opened if
        /// needed, the record is bound at `buffer(3)` for the task and mesh entries' drawId and drawn natively from its group
        /// counts at offset 4, with the bound pipeline's threadgroup sizes.
        void drawLoneMesh(CommandListImpl& list, uint64_t record)
        {
            MTL4::RenderCommandEncoder* encoder = metal4::ensureRender(list);
            metal4::applyDrawRecord(list, record);
            const metal4::PipelineState& pipeline = *list.draw.pipeline;
            {
                // The validation layer autoreleases in a draw it checks.
                const metal4::AutoreleasePool pool;
                encoder->drawMeshThreadgroups(record + offsetof(DrawMeshTasksIndirectArgs, x), pipeline.objectThreads, pipeline.meshThreads);
            }
            core::noteNativeCall(list.core);
            list.encoders.markWork();
            const bool task = pipeline.objectThreads.width != 0;
            list.encoders.markRan(MTL::StageMesh | MTL::StageFragment | (task ? MTL::StageObject : MTL::Stages{ 0 }));
        }
    } // namespace

    //==================================================================================================================
    void CommandList::drawIndirect(Addr args, uint32_t maxCount, Addr count)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::drawIndirect(list.core, args, maxCount, count);
        recordCounted(
            list,
            Counted{ .kind = metal4::IcbKind::Draw,
                     .records = addressOf(args),
                     .maxCount = maxCount,
                     .count = addressOf(count),
                     .indices = {},
                     .indexType = MTL::IndexTypeUInt16 }
        );
    }

    //==================================================================================================================
    void CommandList::drawIndexedIndirect(Buffer indices, IndexType type, Addr args, uint32_t maxCount, Addr count)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::drawIndexedIndirect(list.core, indices, args, maxCount, count);
        recordCounted(
            list,
            Counted{ .kind = metal4::IcbKind::Indexed,
                     .records = addressOf(args),
                     .maxCount = maxCount,
                     .count = addressOf(count),
                     .indices = indices,
                     .indexType = metal4::indexType(type) }
        );
    }

    //==================================================================================================================
    void CommandList::drawMeshTasksIndirect(Addr args, uint32_t maxCount, Addr count)
    {
        CommandListImpl& list = CommandListImpl::of(*this);
        core::drawMeshTasksIndirect(list.core, args, maxCount, count);
        // Every indirect mesh draw is counted except a lone record with no count, which Metal draws natively.
        if (maxCount == 1 && count.lo == 0 && count.hi == 0)
        {
            drawLoneMesh(list, addressOf(args));
            return;
        }
        recordCountedMesh(list, addressOf(args), maxCount, addressOf(count));
    }
} // namespace rojoRHI
