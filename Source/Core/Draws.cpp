//----------------------------------------------------------------------------------------------------------------------
/// @file Draws.cpp
/// @brief Implements the checks of the root, draw, dispatch and indirect command rules.
//----------------------------------------------------------------------------------------------------------------------

#include "Core/Draws.h"

#include <rojoRHI/IndirectArgs.h>

#include "Base/Assert.h"
#include "Core/Addresses.h"
#include "Core/Pipelines.h"

#include <format>
#include <string>

namespace rojoRHI::core {
    namespace {
        /// The byte alignment VU-rojo-abi-0002 requires of a root address: the rule's own number, which every backend's
        /// `Limits::rootAlignment` equals.
        [[maybe_unused]] constexpr uint64_t kRootAlignment = 256;

        //==============================================================================================================
        [[maybe_unused]] bool hasUsage(BufferUsage usage, BufferUsage flag)
        {
            return (usage & flag) != BufferUsage::None;
        }

        //==============================================================================================================
        /// Says what keeps `list` from drawing with a graphics pipeline that has a `stage` stage, or returns an empty string.
        /// `needsViewport` adds the rule that a viewport and a scissor were set since the pass began.
        [[maybe_unused]] std::string drawProblem(const CommandListCore& list, ShaderStage stage, bool needsViewport)
        {
            if (!list.passOpen)
            {
                return "no render pass is open";
            }
            if (!list.pipeline.bound || list.pipeline.kind != PipelineKind::Graphics)
            {
                return "no graphics pipeline is bound";
            }
            if ((list.pipeline.stages & stageBit(stage)) == 0)
            {
                return std::format("the bound pipeline has no {} stage", stageName(stage));
            }
            if (needsViewport && !(list.pass.viewportSet && list.pass.scissorSet))
            {
                return "no viewport and scissor were set since the pass began";
            }
            return {};
        }

        //==============================================================================================================
        /// Says what keeps `list` from dispatching, or returns an empty string.
        [[maybe_unused]] std::string dispatchProblem(const CommandListCore& list)
        {
            if (list.passOpen)
            {
                return "a render pass is open";
            }
            if (list.type == QueueType::Copy)
            {
                return "the list is a Copy list";
            }
            if (!list.pipeline.bound || list.pipeline.kind != PipelineKind::Compute)
            {
                return "no compute pipeline is bound";
            }
            return {};
        }

        //==============================================================================================================
        /// Says why `at` is not a 4-byte aligned run of `bytes` bytes inside one `IndirectArgs` buffer, or returns an empty string.
        [[maybe_unused]] std::string indirectProblem(const ResolvedAddr& at, uint64_t bytes)
        {
            if (at.record == nullptr)
            {
                return "the address is null";
            }
            if (at.offset % 4 != 0)
            {
                return std::format("the byte offset {} is not a multiple of 4", at.offset);
            }
            if (!hasUsage(at.record->usage, BufferUsage::IndirectArgs))
            {
                return std::format("the buffer \"{}\" was not created with IndirectArgs", at.record->label);
            }
            if (at.offset + bytes > at.record->size)
            {
                return std::format("{} bytes from offset {} leave the buffer \"{}\" of {} bytes", bytes, at.offset, at.record->label, at.record->size);
            }
            return {};
        }

        //==============================================================================================================
        [[maybe_unused]] uint64_t indexBytes(IndexType type)
        {
            return type == IndexType::UInt16 ? 2 : 4;
        }
    } // namespace

    //==================================================================================================================
    void setRoot(CommandListCore& list, Addr root)
    {
        checkRecording(list);
        ROJORHI_VU((root.lo != 0 || root.hi != 0), "VU-rojo-abi-0001", "setRoot was given the null address");
#ifndef NDEBUG
        // Resolving reads the buffer pool under its lock, so only the Debug tier does it (ADR 0019).
        const ResolvedAddr at = resolveAddr(*list.device, list, root);
        ROJORHI_VU(at.offset % kRootAlignment == 0, "VU-rojo-abi-0002", "the root's byte offset {} is not a multiple of {}", at.offset, kRootAlignment);
#endif
        list.root = root;
    }

    //==================================================================================================================
    void draw(CommandListCore& list)
    {
        checkRecording(list);
        ROJORHI_VU(drawProblem(list, ShaderStage::Vertex, false).empty(), "VU-rojo-cmd-0023", "draw: {}", drawProblem(list, ShaderStage::Vertex, false));
        ROJORHI_VU(list.pass.viewportSet && list.pass.scissorSet, "VU-rojo-cmd-0024", "draw: no viewport and scissor were set since the pass began");
    }

    //==================================================================================================================
    void drawIndexed(
        CommandListCore& list,
        [[maybe_unused]] Buffer indices,
        [[maybe_unused]] IndexType type,
        [[maybe_unused]] uint32_t count,
        [[maybe_unused]] uint32_t firstIndex
    )
    {
        checkRecording(list);
        ROJORHI_VU(drawProblem(list, ShaderStage::Vertex, true).empty(), "VU-rojo-cmd-0025", "drawIndexed: {}", drawProblem(list, ShaderStage::Vertex, true));
#ifndef NDEBUG
        // The record feeds the Debug-tier check only; reading it takes the buffer pool's lock (ADR 0019).
        const BufferRecord* buffer = list.device->pool<Buffer>().checked(indices, false);
        ROJORHI_VU(
            hasUsage(buffer->usage, BufferUsage::Index) && (uint64_t{ firstIndex } + count) * indexBytes(type) <= buffer->size,
            "VU-rojo-cmd-0026",
            "drawIndexed reads indices {} to {} of the buffer \"{}\", which has {} bytes and {}the Index usage",
            firstIndex,
            uint64_t{ firstIndex } + count,
            buffer->label,
            buffer->size,
            hasUsage(buffer->usage, BufferUsage::Index) ? "" : "not "
        );
#endif
    }

    //==================================================================================================================
    void drawMeshTasks(CommandListCore& list, [[maybe_unused]] uint32_t x, [[maybe_unused]] uint32_t y, [[maybe_unused]] uint32_t z)
    {
        checkRecording(list);
        ROJORHI_VU(drawProblem(list, ShaderStage::Mesh, true).empty(), "VU-rojo-cmd-0027", "drawMeshTasks: {}", drawProblem(list, ShaderStage::Mesh, true));
        [[maybe_unused]] const Limits& limits = list.device->capabilities().limits;
        ROJORHI_VU(
            x <= limits.maxMeshTaskGroups && y <= limits.maxMeshTaskGroups && z <= limits.maxMeshTaskGroups &&
                uint64_t{ x } * y * z <= limits.maxMeshTaskGroupsTotal,
            "VU-rojo-cmd-0028",
            "drawMeshTasks launched {} by {} by {} groups, above the limits {} per dimension and {} in total",
            x,
            y,
            z,
            limits.maxMeshTaskGroups,
            limits.maxMeshTaskGroupsTotal
        );
    }

    //==================================================================================================================
    void dispatch(CommandListCore& list, [[maybe_unused]] uint32_t x, [[maybe_unused]] uint32_t y, [[maybe_unused]] uint32_t z)
    {
        checkRecording(list);
        ROJORHI_VU(!list.passOpen && list.type != QueueType::Copy, "VU-rojo-cmd-0029", "dispatch: {}", dispatchProblem(list));
        ROJORHI_VU(list.pipeline.bound && list.pipeline.kind == PipelineKind::Compute, "VU-rojo-cmd-0030", "dispatch: no compute pipeline is bound");
        [[maybe_unused]] const uint32_t maxGroups = list.device->capabilities().limits.maxDispatchGroups;
        ROJORHI_VU(
            x <= maxGroups && y <= maxGroups && z <= maxGroups,
            "VU-rojo-cmd-0031",
            "dispatch launched {} by {} by {} groups, above the limit {}",
            x,
            y,
            z,
            maxGroups
        );
    }

    //==================================================================================================================
    void drawIndirect(CommandListCore& list, [[maybe_unused]] Addr args, [[maybe_unused]] uint32_t maxCount, [[maybe_unused]] Addr count)
    {
        checkRecording(list);
        ROJORHI_VU(
            drawProblem(list, ShaderStage::Vertex, false).empty(), "VU-rojo-indirect-0001", "drawIndirect: {}", drawProblem(list, ShaderStage::Vertex, false)
        );
        ROJORHI_VU(
            list.pass.viewportSet && list.pass.scissorSet, "VU-rojo-indirect-0021", "drawIndirect: no viewport and scissor were set since the pass began"
        );
#ifndef NDEBUG
        // Resolving the addresses reads the buffer pool under its lock, so only the Debug tier does it (ADR 0019).
        const ResolvedAddr first = resolveAddr(*list.device, list, args);
        const ResolvedAddr counted = resolveAddr(*list.device, list, count);
        const uint64_t bytes = uint64_t{ maxCount } * sizeof(DrawIndirectArgs);
        ROJORHI_VU(indirectProblem(first, bytes).empty(), "VU-rojo-indirect-0002", "drawIndirect args: {}", indirectProblem(first, bytes));
        ROJORHI_VU(
            maxCount > 0 && maxCount <= list.device->capabilities().limits.maxDrawIndirectCount,
            "VU-rojo-indirect-0003",
            "drawIndirect maxCount {} is not in 1 to {}",
            maxCount,
            list.device->capabilities().limits.maxDrawIndirectCount
        );
        ROJORHI_VU(
            counted.record == nullptr || indirectProblem(counted, sizeof(uint32_t)).empty(),
            "VU-rojo-indirect-0004",
            "drawIndirect count: {}",
            indirectProblem(counted, sizeof(uint32_t))
        );
#endif
    }

    //==================================================================================================================
    void drawIndexedIndirect(
        CommandListCore& list, [[maybe_unused]] Buffer indices, [[maybe_unused]] Addr args, [[maybe_unused]] uint32_t maxCount, [[maybe_unused]] Addr count
    )
    {
        checkRecording(list);
        ROJORHI_VU(
            drawProblem(list, ShaderStage::Vertex, false).empty(),
            "VU-rojo-indirect-0006",
            "drawIndexedIndirect: {}",
            drawProblem(list, ShaderStage::Vertex, false)
        );
        ROJORHI_VU(
            list.pass.viewportSet && list.pass.scissorSet, "VU-rojo-indirect-0022", "drawIndexedIndirect: no viewport and scissor were set since the pass began"
        );
#ifndef NDEBUG
        // The records feed the Debug-tier checks only; reading them takes the buffer pool's lock (ADR 0019).
        const BufferRecord* buffer = list.device->pool<Buffer>().checked(indices, false);
        ROJORHI_VU(
            hasUsage(buffer->usage, BufferUsage::Index),
            "VU-rojo-indirect-0007",
            "drawIndexedIndirect: the buffer \"{}\" was not created with Index",
            buffer->label
        );
        const ResolvedAddr first = resolveAddr(*list.device, list, args);
        const ResolvedAddr counted = resolveAddr(*list.device, list, count);
        const uint64_t bytes = uint64_t{ maxCount } * sizeof(DrawIndexedIndirectArgs);
        ROJORHI_VU(indirectProblem(first, bytes).empty(), "VU-rojo-indirect-0008", "drawIndexedIndirect args: {}", indirectProblem(first, bytes));
        ROJORHI_VU(
            maxCount > 0 && maxCount <= list.device->capabilities().limits.maxDrawIndirectCount,
            "VU-rojo-indirect-0009",
            "drawIndexedIndirect maxCount {} is not in 1 to {}",
            maxCount,
            list.device->capabilities().limits.maxDrawIndirectCount
        );
        ROJORHI_VU(
            counted.record == nullptr || indirectProblem(counted, sizeof(uint32_t)).empty(),
            "VU-rojo-indirect-0010",
            "drawIndexedIndirect count: {}",
            indirectProblem(counted, sizeof(uint32_t))
        );
#endif
    }

    //==================================================================================================================
    void drawMeshTasksIndirect(CommandListCore& list, [[maybe_unused]] Addr args, [[maybe_unused]] uint32_t maxCount, [[maybe_unused]] Addr count)
    {
        checkRecording(list);
        ROJORHI_VU(
            drawProblem(list, ShaderStage::Mesh, false).empty(),
            "VU-rojo-indirect-0012",
            "drawMeshTasksIndirect: {}",
            drawProblem(list, ShaderStage::Mesh, false)
        );
        ROJORHI_VU(
            list.pass.viewportSet && list.pass.scissorSet,
            "VU-rojo-indirect-0023",
            "drawMeshTasksIndirect: no viewport and scissor were set since the pass began"
        );
        ROJORHI_VU(
            list.device->capabilities().meshTasksIndirect, "VU-rojo-indirect-0013", "drawMeshTasksIndirect was recorded on a device without meshTasksIndirect"
        );
#ifndef NDEBUG
        // Resolving the addresses reads the buffer pool under its lock, so only the Debug tier does it (ADR 0019).
        const ResolvedAddr first = resolveAddr(*list.device, list, args);
        const ResolvedAddr counted = resolveAddr(*list.device, list, count);
        const uint64_t bytes = uint64_t{ maxCount } * sizeof(DrawMeshTasksIndirectArgs);
        ROJORHI_VU(indirectProblem(first, bytes).empty(), "VU-rojo-indirect-0014", "drawMeshTasksIndirect args: {}", indirectProblem(first, bytes));
        ROJORHI_VU(
            maxCount > 0 && maxCount <= list.device->capabilities().limits.maxDrawIndirectCount,
            "VU-rojo-indirect-0015",
            "drawMeshTasksIndirect maxCount {} is not in 1 to {}",
            maxCount,
            list.device->capabilities().limits.maxDrawIndirectCount
        );
        ROJORHI_VU(
            counted.record == nullptr || indirectProblem(counted, sizeof(uint32_t)).empty(),
            "VU-rojo-indirect-0016",
            "drawMeshTasksIndirect count: {}",
            indirectProblem(counted, sizeof(uint32_t))
        );
#endif
    }

    //==================================================================================================================
    void dispatchIndirect(CommandListCore& list, [[maybe_unused]] Addr args)
    {
        checkRecording(list);
        ROJORHI_VU(
            !list.passOpen && list.pipeline.bound && list.pipeline.kind == PipelineKind::Compute,
            "VU-rojo-indirect-0018",
            "dispatchIndirect: {}",
            dispatchProblem(list)
        );
#ifndef NDEBUG
        // Resolving the addresses reads the buffer pool under its lock, so only the Debug tier does it (ADR 0019).
        const ResolvedAddr first = resolveAddr(*list.device, list, args);
        ROJORHI_VU(
            indirectProblem(first, sizeof(DispatchIndirectArgs)).empty(),
            "VU-rojo-indirect-0019",
            "dispatchIndirect args: {}",
            indirectProblem(first, sizeof(DispatchIndirectArgs))
        );
#endif
    }
} // namespace rojoRHI::core
