//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Encoders.cpp
/// @brief Implements the encoder a Metal 4 command list keeps open: the lazy compute encoder and the render encoders, their opening and closing.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Encoders.h"

#include "Base/Assert.h"
#include "Core/CommandLists.h"
#include "Metal4Barriers.h"
#include "Metal4Common.h"
#include "Metal4Kernels.h"
#include "Metal4ListState.h"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <atomic>

namespace rojoRHI::metal4 {
    MTL4::ComputeCommandEncoder* Encoders::prepareCompute(core::CommandListCore& list, ListState& state, MTL::Stages command, bool kernelsOnly)
    {
        if (m_lingering)
        {
            close(list);
        }
        ROJORHI_INTERNAL_ASSERT(m_kind != EncoderKind::Render, "a compute command is recorded while a render encoder is open");
        if (m_compute != nullptr)
        {
            if (!m_holdsWork && (m_barriers.pendingDestination & kComputeStages) != 0)
            {
                // An encoder opened with no command, as binding a compute pipeline opens it, may meet a barrier before its
                // first command: the barrier merged into the pending one, which this encoder owes before any command. It
                // waits at queue level like the opening barrier.
                const MTL::Stages source = m_barriers.openSource;
                const MTL::Stages destination = m_barriers.openDestination;
                const MTL4::VisibilityOptions visibility = m_barriers.openVisibility;
                const bool second = destination != 0 || m_barriers.mergedDestination != 0;
                if (second)
                {
                    // The chain rule orders this barrier's destinations after what the earlier opening waited for too, when its
                    // sources name that opening's destinations: the sources it waited for join this one's. Widening only waits
                    // more.
                    m_barriers.pendingSource |= source | m_barriers.mergedSource;
                }
                emitAtOpen(list, m_barriers, m_compute, kComputeStages);
                if (second)
                {
                    // Two barriers order S1 -> D1 and S2 -> D2, which is less than their unions: the encoder records no coverage
                    // on their strength. What they held back still returns to the pending barrier if no command ran in it.
                    m_barriers.mergeOpened(source, destination, visibility);
                }
            }
            waitCarry(list, command);
            // A command follows: the encoder-level barrier held back goes before it when it holds the command's stage back.
            emitDeferred(list, m_barriers, m_compute, kComputeStages, command);
            return m_compute;
        }
        {
            // The factory autoreleases its encoder and the driver the label's text; the list keeps the encoder, so it retains it.
            const AutoreleasePool pool;
            m_compute = state.commandBuffer->computeCommandEncoder();
            m_compute->retain();
            if (state.markers)
            {
                m_compute->setLabel(state.scopeLabels.empty() ? state.label : state.scopeLabels.back());
                core::noteNativeCall(list);
            }
        }
        core::noteNativeCall(list);
        ++list.counts.encodersOpened;
        if (!kernelsOnly)
        {
            m_compute->setArgumentTable(state.table);
            core::noteNativeCall(list);
        }
        // The pending barrier's part on the encoder's stages waits at queue level before any of its commands.
        emitAtOpen(list, m_barriers, m_compute, kComputeStages);
        m_kind = EncoderKind::Compute;
        m_holdsWork = false;
        m_kernelPipeline = nullptr;
        m_kernelsOnly = kernelsOnly;
        if (kernelsOnly && m_kernelsRan != nullptr && !m_kernelsRan->load(std::memory_order_relaxed))
        {
            // From the next frame on, the queue's lists spare these kernels a barrier to the dispatch stage.
            m_kernelsRan->store(true, std::memory_order_relaxed);
        }
        m_carryWaited = false;
        m_commandPools = state.commandPools;
        waitCarry(list, command);
        return m_compute;
    }

    void Encoders::queueKernel(core::CommandListCore& list, const ListState& state, const MTL::ComputePipelineState* kernel, uint64_t params, uint64_t stride)
    {
        ROJORHI_INTERNAL_ASSERT(m_kind == EncoderKind::Compute && m_kernelsOnly, "a writer kernel is queued with no kernels-only encoder open");
        if (m_run.count != 0 && m_run.kernel == kernel && m_run.stride == stride && params == m_run.params + m_run.count * stride)
        {
            ++m_run.count;
            return;
        }
        dispatchRun(list);
        m_run = KernelRun{ .kernel = kernel, .state = &state, .params = params, .stride = stride, .count = 1 };
    }

    void Encoders::dispatchRun(core::CommandListCore& list)
    {
        if (m_run.count == 0)
        {
            return;
        }
        dispatchKernelRun(list, *this, m_run);
        m_run = KernelRun{};
    }

    void Encoders::waitCarry(core::CommandListCore& list, MTL::Stages command)
    {
        if (!owesCarryWait(command))
        {
            return;
        }
        // The wait holds back this encoder's later dispatches until every earlier update of the fence on the queue, each made
        // by an earlier list's last encoder after the work its barrier still owed. A wait orders only the work of its own
        // encoder, so each encoder that dispatches waits, before its first dispatch.
        m_compute->waitForFence(m_carryFence, MTL::StageDispatch);
        core::noteNativeCall(list);
        m_carryWaited = true;
    }

    bool Encoders::barrierInOpen(core::CommandListCore& list, MTL::Stages source, MTL::Stages destination, MTL4::VisibilityOptions visibility)
    {
        if (m_kind != EncoderKind::Compute || m_kernelsOnly || m_barriers.openDestination != 0 || m_barriers.mergedDestination != 0)
        {
            return false;
        }
        // A batch still pending waits for sources the pair would not name, and a chain through it needs them in one barrier,
        // as the merge into the pending barrier gives.
        if (m_barriers.pendingDestination != 0)
        {
            return false;
        }
        // stages() reads the stages the encoder's commands ran in; it records nothing. A command already run in a destination
        // stage lies before the barrier: the queue-level part, which orders the commands encoded after it, still orders the
        // later ones, and the encoder-level part orders the earlier ones, but the pair no longer stands for the opening barrier
        // of those stages until a command in them follows (`preRan`).
        const MTL::Stages before = m_compute->stages() & destination;
        // The queue-level barrier orders only earlier encoders' work, and Metal may apply it earlier in the encoder, which
        // only waits more; the encoder-level part orders the encoder's own work before its next command in `destination`.
        m_compute->barrierAfterQueueStages(source, destination, visibility);
        core::noteNativeCall(list);
        ++list.counts.nativeBarriers;
        m_barriers.openSource = source;
        m_barriers.openDestination = destination;
        m_barriers.openVisibility = visibility;
        m_barriers.preRan = before;
        m_barriers.defer(source, destination, visibility);
        return true;
    }

    MTL4::RenderCommandEncoder* Encoders::openRender(core::CommandListCore& list, ListState& state, const MTL4::RenderPassDescriptor* descriptor)
    {
        if (m_lingering)
        {
            close(list);
        }
        ROJORHI_INTERNAL_ASSERT(m_kind == EncoderKind::None, "a render encoder opens while another encoder is open");
        {
            // The factory autoreleases its encoder and the driver the label's text; the list keeps the encoder, so it retains it.
            const AutoreleasePool pool;
            m_render = state.commandBuffer->renderCommandEncoder(descriptor);
            m_render->retain();
            if (state.markers)
            {
                m_render->setLabel(state.scopeLabels.empty() ? state.label : state.scopeLabels.back());
                core::noteNativeCall(list);
            }
        }
        m_commandPools = state.commandPools;
        core::noteNativeCall(list);
        ++list.counts.encodersOpened;
        // The pending barrier's part on the fragment stage, where the load and store actions run, waits at queue level before
        // them. The other render stages stay owed to the next encoder that runs them, so a clear of many subresources emits
        // the barrier once.
        emitAtOpen(list, m_barriers, m_render, MTL::StageFragment);
        m_kind = EncoderKind::Render;
        m_holdsWork = false;
        m_ran = MTL::StageFragment;
        m_tableBound = false;
        return m_render;
    }

    MTL4::RenderCommandEncoder* Encoders::openPass(core::CommandListCore& list, ListState& state, const MTL4::RenderPassDescriptor* descriptor)
    {
        if (m_lingering)
        {
            close(list);
        }
        ROJORHI_INTERNAL_ASSERT(m_kind == EncoderKind::None, "a render pass's encoder opens while another encoder is open");
        // The factory autoreleases its encoder and the driver the label's text, into the caller's pool; the list keeps the
        // encoder, so it retains it.
        m_render = state.commandBuffer->renderCommandEncoder(descriptor);
        m_render->retain();
        if (state.markers)
        {
            m_render->setLabel(state.scopeLabels.empty() ? state.label : state.scopeLabels.back());
            core::noteNativeCall(list);
        }
        m_commandPools = state.commandPools;
        core::noteNativeCall(list);
        ++list.counts.encodersOpened;
        // The vertex and fragment stages take the list's table before the encoder's first draw that reads it (`bindTable`),
        // and the object and mesh stages at its first mesh pipeline (`applyPipeline`), since each bound stage costs every
        // draw after a table write.
        m_tableBound = false;
        // The pending barrier's part on every render stage waits at queue level before the pass's load actions and draws.
        emitAtOpen(list, m_barriers, m_render, kRenderStages);
        m_kind = EncoderKind::Render;
        m_holdsWork = false;
        m_ran = MTL::StageFragment;
        return m_render;
    }

    void Encoders::carry(core::CommandListCore& list, ListState& state, MTL::Fence* fence)
    {
        if (m_lingering)
        {
            close(list);
        }
        ROJORHI_INTERNAL_ASSERT(m_kind == EncoderKind::None, "an upscale's carrier opens while another encoder is open");
        // The factory autoreleases its encoder, which is used and ended inside the pool.
        const AutoreleasePool pool;
        MTL4::ComputeCommandEncoder* carrier = state.commandBuffer->computeCommandEncoder();
        core::noteNativeCall(list);
        ++list.counts.encodersOpened;
        if (state.markers)
        {
            carrier->setLabel(state.scopeLabels.empty() ? state.label : state.scopeLabels.back());
            core::noteNativeCall(list);
        }
        if (m_barriers.owedFenceStages != 0)
        {
            // An earlier upscale's fence still owed is waited on here too, so this upscale's fence, which replaces it, passes
            // only after that upscale's work.
            carrier->waitForFence(m_barriers.owedFence, kComputeStages);
            core::noteNativeCall(list);
        }
        // MetalFX reads its history and scratch resources, which an earlier upscale wrote, in this list or an earlier command
        // buffer, at stages no barrier of the caller names: the carrier waits for every stage of all earlier work on the
        // queue, which also covers the pending barrier's part on its own stages, with the pending visibility.
        carrier->barrierAfterQueueStages(MTL::StageAll, kComputeStages, m_barriers.pendingVisibility | MTL4::VisibilityOptionDevice);
        core::noteNativeCall(list);
        ++list.counts.nativeBarriers;
        m_barriers.pendingDestination &= ~kComputeStages;
        if (m_barriers.pendingDestination == 0)
        {
            m_barriers.pendingSource = 0;
            m_barriers.pendingVisibility = 0;
        }
        // The carrier runs no command, so the fence passes once its stages have waited for all earlier work.
        carrier->updateFence(fence, kComputeStages);
        core::noteNativeCall(list);
        carrier->endEncoding();
        core::noteNativeCall(list);
        m_barriers.clearOpen();
    }

    void Encoders::settle()
    {
        // An encoder-level barrier still held back orders nothing in later encoders, so it waits for them at queue level.
        m_barriers.queueDeferred();
        m_barriers.returnUnrun(ranSinceOpen());
    }

    MTL::Stages Encoders::ranSinceOpen() const
    {
        return ranStages() & ~m_barriers.preRan;
    }

    MTL::Stages Encoders::ranStages() const
    {
        // stages() reads the stages the compute encoder's commands ran in; it records nothing. A render encoder has no stages
        // getter, so it keeps those it ran in: its load and store actions are fragment-stage work, and a pass's commands add
        // theirs.
        return m_kind == EncoderKind::Compute ? m_compute->stages() : m_ran;
    }

    void Encoders::linger()
    {
        ROJORHI_INTERNAL_ASSERT(m_kind == EncoderKind::Render && !m_lingering, "a render pass's encoder lingers with none open");
        settle();
        m_lingering = true;
    }

    void Encoders::finish(core::CommandListCore& list)
    {
        if (m_kind == EncoderKind::None)
        {
            return;
        }
        dispatchRun(list);
        if (!m_lingering)
        {
            settle();
        }
        BarrierState& barriers = m_barriers;
        const MTL::Stages ran = ranStages();
        const MTL::Stages ranSince = ranSinceOpen();
        // Every source stage the encoder's opening barrier waited for at queue level lies before a command the encoder ran in
        // a stage that barrier held, and any other source work lies in the encoder: once the stages it ran complete, every
        // source the pending barrier names has completed, in this list and in earlier ones. A list that upscaled owes
        // MetalFX's work, which only its fence orders, so its trailing encoder emits what is owed.
        const bool covered = (barriers.pendingSource & ~barriers.openSource) == 0 && (barriers.openDestination & ranSince) != 0;
        if (barriers.pendingDestination != 0 && barriers.owedFence == nullptr && covered)
        {
            MTL4::CommandEncoder* encoder =
                m_kind == EncoderKind::Compute ? static_cast<MTL4::CommandEncoder*>(m_compute) : static_cast<MTL4::CommandEncoder*>(m_render);
            const MTL::Stages after = (barriers.pendingSource & ran) | (barriers.openDestination & ranSince);
            // Later dispatches wait on the fence, and a counted draw's kernels, which read only records an IndirectArgs barrier
            // orders, do not: a barrier from these stages to the dispatch stage would hold them back too.
            const MTL::Stages carried = fencedStages(barriers, m_carryFence);
            MTL::Stages fenceAfter = carried != 0 ? after : 0;
            MTL::Stages barrierAfter = 0;
            MTL::Stages barrierDestination = barriers.pendingDestination & ~carried;
            MTL4::VisibilityOptions barrierVisibility = 0;
            if (barrierDestination != 0)
            {
                barrierAfter = after;
                barrierVisibility = barriers.pendingVisibility;
            }
            // The work the encoder's kept barriers spent is owed a producer barrier too, unless this carry names it already.
            // It folds in, so a list end that owed both emits one: the part the fence may carry joins the fence update, which
            // has no visibility option, and the rest joins the barrier. Each source named is a stage the encoder ran, so the
            // union orders every pair its parts ordered.
            const MTL::Stages keptAfter = barriers.keptSource & ran;
            if (keptAfter != 0 && barriers.keptDestination != 0 && !barriers.keptWithin(ran, after, barriers.pendingDestination, barriers.pendingVisibility))
            {
                const MTL::Stages keptCarried = fencedStages(barriers, m_carryFence, barriers.keptDestination, barriers.keptVisibility);
                const MTL::Stages keptRest = barriers.keptDestination & ~keptCarried;
                if (keptCarried != 0)
                {
                    fenceAfter |= keptAfter;
                }
                if (keptRest != 0)
                {
                    barrierAfter |= keptAfter;
                    barrierDestination |= keptRest;
                    barrierVisibility |= barriers.keptVisibility;
                }
            }
            if (fenceAfter != 0)
            {
                encoder->updateFence(m_carryFence, fenceAfter);
                core::noteNativeCall(list);
                m_syncsAtEnd = true;
            }
            if (barrierDestination != 0)
            {
                // Every later encoder on the queue, in a later list, waits in these stages for the encoder's work.
                encoder->barrierAfterStages(barrierAfter, barrierDestination, barrierVisibility);
                core::noteNativeCall(list);
                ++list.counts.nativeBarriers;
                m_syncsAtEnd = true;
            }
            // The carry orders the work the encoder's kept barriers spent as well: folded in, or named by it already.
            barriers.clearKept();
            barriers.pendingSource = 0;
            barriers.pendingDestination = 0;
            barriers.pendingVisibility = 0;
        }
        // Already settled: close only ends the encoder.
        m_lingering = true;
        close(list);
    }

    void Encoders::close(core::CommandListCore& list)
    {
        if (m_kind == EncoderKind::None)
        {
            return;
        }
        // The run's dispatch is the encoder's last command, so the encoder's stages and barriers see it.
        dispatchRun(list);
        if (!m_lingering)
        {
            settle();
        }
        if (m_kind == EncoderKind::Compute)
        {
            endKept(list);
            {
                // The validation layer autoreleases when a compute encoder that waited on the carry fence, updated it or
                // holds back later encoders ends; without it nothing does. Only its first dispatch and the carry at `end`
                // bring that about, once per encoder.
                const CommandPool pool(m_commandPools && (m_carryWaited || m_syncsAtEnd));
                m_compute->endEncoding();
            }
            core::noteNativeCall(list);
            m_ended.push_back(m_compute);
            m_compute = nullptr;
        }
        else
        {
            // The validation layer autoreleases in endEncoding of a render encoder with a stencil attachment.
            const CommandPool pool(m_commandPools);
            m_render->endEncoding();
            core::noteNativeCall(list);
            m_ended.push_back(m_render);
            m_render = nullptr;
        }
        m_kind = EncoderKind::None;
        m_holdsWork = false;
        m_ran = 0;
        m_lingering = false;
        m_kernelsOnly = false;
        m_carryWaited = false;
        m_syncsAtEnd = false;
        m_kernelPipeline = nullptr;
        m_barriers.clearOpen();
        m_barriers.clearKept();
    }

    void Encoders::endKept(core::CommandListCore& list)
    {
        const BarrierState& barriers = m_barriers;
        const MTL::Stages after = barriers.keptSource & m_compute->stages();
        // A barrier the encoder kept orders its own later commands only, and a command of the encoder has spent it, so
        // nothing orders the work it waited for before the later encoders: one producer barrier does, unless the barrier
        // still pending already names that work, which the next consumer barrier then covers as an earlier pass.
        if (after == 0 || barriers.keptDestination == 0 ||
            barriers.keptWithin(m_compute->stages(), barriers.pendingSource, barriers.pendingDestination, barriers.pendingVisibility))
        {
            return;
        }
        m_compute->barrierAfterStages(after, barriers.keptDestination, barriers.keptVisibility);
        core::noteNativeCall(list);
        ++list.counts.nativeBarriers;
        m_syncsAtEnd = true;
    }

    void Encoders::releaseEnded()
    {
        for (MTL4::CommandEncoder* encoder : m_ended)
        {
            encoder->release();
        }
        m_ended.clear();
    }

    void Encoders::release()
    {
        if (m_compute != nullptr || m_render != nullptr || !m_ended.empty())
        {
            const AutoreleasePool pool;
            if (m_compute != nullptr)
            {
                m_compute->release();
            }
            if (m_render != nullptr)
            {
                m_render->release();
            }
            releaseEnded();
        }
        m_compute = nullptr;
        m_render = nullptr;
        m_kind = EncoderKind::None;
        m_holdsWork = false;
        m_ran = 0;
        m_lingering = false;
        m_kernelsOnly = false;
        m_carryWaited = false;
        m_syncsAtEnd = false;
        m_kernelPipeline = nullptr;
        m_run = KernelRun{};
        m_barriers.reset();
    }
} // namespace rojoRHI::metal4
