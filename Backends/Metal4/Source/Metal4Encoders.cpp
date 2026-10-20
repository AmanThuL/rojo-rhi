//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Encoders.cpp
/// @brief Implements the encoder a Metal 4 command list keeps open: the lazy compute encoder and the render encoders, their opening and closing.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Encoders.h"

#include "Base/Assert.h"
#include "Core/CommandLists.h"
#include "Metal4Barriers.h"
#include "Metal4Common.h"
#include "Metal4ListState.h"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

namespace rojoRHI::metal4 {
    //==================================================================================================================
    MTL4::ComputeCommandEncoder* Encoders::compute(core::CommandListCore& list, ListState& state, MTL::Stages command, bool kernelsOnly)
    {
        ROJORHI_INTERNAL_ASSERT(m_kind != EncoderKind::Render, "a compute command is recorded while a render encoder is open");
        if (m_compute != nullptr)
        {
            if (!m_holdsWork && (m_barriers.pendingDestination & kComputeStages) != 0)
            {
                // An encoder opened with no command, as binding a compute pipeline opens it, may meet a barrier before its
                // first command: the barrier merged into the pending one, which this encoder owes before any command. It
                // waits at queue level like the opening barrier, and what it covers joins what the opening covered.
                const MTL::Stages source = m_barriers.openSource;
                const MTL::Stages destination = m_barriers.openDestination;
                const MTL4::VisibilityOptions visibility = m_barriers.openVisibility;
                emitAtOpen(list, m_barriers, m_compute, kComputeStages);
                m_barriers.openSource |= source;
                m_barriers.openDestination |= destination;
                m_barriers.openVisibility |= visibility;
            }
            // A command follows: the encoder-level barrier held back goes before it when it holds the command's stage back.
            emitDeferred(list, m_barriers, m_compute, kComputeStages, command);
            return m_compute;
        }
        {
            // The factory autoreleases its encoder; the list keeps it past the pool, so it retains it.
            const AutoreleasePool pool;
            m_compute = state.commandBuffer->computeCommandEncoder();
            m_compute->retain();
        }
        core::noteNativeCall(list);
        ++list.counts.encodersOpened;
        m_compute->setLabel(state.scopeLabels.empty() ? state.label : state.scopeLabels.back());
        core::noteNativeCall(list);
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
        return m_compute;
    }

    //==================================================================================================================
    MTL4::RenderCommandEncoder* Encoders::openRender(core::CommandListCore& list, ListState& state, const MTL4::RenderPassDescriptor* descriptor)
    {
        ROJORHI_INTERNAL_ASSERT(m_kind == EncoderKind::None, "a render encoder opens while another encoder is open");
        {
            // The factory autoreleases its encoder; the list keeps it past the pool, so it retains it.
            const AutoreleasePool pool;
            m_render = state.commandBuffer->renderCommandEncoder(descriptor);
            m_render->retain();
        }
        m_commandPools = state.commandPools;
        core::noteNativeCall(list);
        ++list.counts.encodersOpened;
        m_render->setLabel(state.scopeLabels.empty() ? state.label : state.scopeLabels.back());
        core::noteNativeCall(list);
        // The pending barrier's part on the fragment stage, where the load and store actions run, waits at queue level before
        // them. The other render stages stay owed to the next encoder that runs them, so a clear of many subresources emits
        // the barrier once.
        emitAtOpen(list, m_barriers, m_render, MTL::StageFragment);
        m_kind = EncoderKind::Render;
        m_holdsWork = false;
        m_ran = MTL::StageFragment;
        return m_render;
    }

    //==================================================================================================================
    MTL4::RenderCommandEncoder* Encoders::openPass(core::CommandListCore& list, ListState& state, const MTL4::RenderPassDescriptor* descriptor)
    {
        ROJORHI_INTERNAL_ASSERT(m_kind == EncoderKind::None, "a render pass's encoder opens while another encoder is open");
        {
            // The factory autoreleases its encoder; the list keeps it past the pool, so it retains it.
            const AutoreleasePool pool;
            m_render = state.commandBuffer->renderCommandEncoder(descriptor);
            m_render->retain();
        }
        m_commandPools = state.commandPools;
        core::noteNativeCall(list);
        ++list.counts.encodersOpened;
        m_render->setLabel(state.scopeLabels.empty() ? state.label : state.scopeLabels.back());
        core::noteNativeCall(list);
        // Every shader of a draw reads the reserved bindings through the list's table (ADR 0018).
        m_render->setArgumentTable(state.table, MTL::RenderStageVertex | MTL::RenderStageFragment | MTL::RenderStageObject | MTL::RenderStageMesh);
        core::noteNativeCall(list);
        // The pending barrier's part on every render stage waits at queue level before the pass's load actions and draws.
        emitAtOpen(list, m_barriers, m_render, kRenderStages);
        m_kind = EncoderKind::Render;
        m_holdsWork = false;
        m_ran = MTL::StageFragment;
        return m_render;
    }

    //==================================================================================================================
    void Encoders::carry(core::CommandListCore& list, ListState& state, MTL::Fence* fence)
    {
        ROJORHI_INTERNAL_ASSERT(m_kind == EncoderKind::None, "an upscale's carrier opens while another encoder is open");
        // The factory autoreleases its encoder, which is used and ended inside the pool.
        const AutoreleasePool pool;
        MTL4::ComputeCommandEncoder* carrier = state.commandBuffer->computeCommandEncoder();
        core::noteNativeCall(list);
        ++list.counts.encodersOpened;
        carrier->setLabel(state.scopeLabels.empty() ? state.label : state.scopeLabels.back());
        core::noteNativeCall(list);
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

    //==================================================================================================================
    void Encoders::close(core::CommandListCore& list)
    {
        if (m_kind == EncoderKind::None)
        {
            return;
        }
        // An encoder-level barrier still held back orders nothing in later encoders, so it waits for them at queue level.
        m_barriers.queueDeferred();
        if (m_kind == EncoderKind::Compute)
        {
            // stages() reads the stages the encoder's commands ran in; it records nothing.
            m_barriers.returnUnrun(m_compute->stages());
            m_compute->endEncoding();
            core::noteNativeCall(list);
            m_compute->release();
            m_compute = nullptr;
        }
        else
        {
            // A render encoder has no stages getter, so it keeps those it ran in: its load and store actions are fragment-stage
            // work, and a pass's commands add theirs.
            m_barriers.returnUnrun(m_ran);
            // The validation layer autoreleases in endEncoding of a render encoder with a stencil attachment.
            const CommandPool pool(m_commandPools);
            m_render->endEncoding();
            core::noteNativeCall(list);
            m_render->release();
            m_render = nullptr;
        }
        m_kind = EncoderKind::None;
        m_holdsWork = false;
        m_ran = 0;
        m_kernelPipeline = nullptr;
        m_barriers.clearOpen();
    }

    //==================================================================================================================
    void Encoders::release()
    {
        if (m_compute != nullptr || m_render != nullptr)
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
        }
        m_compute = nullptr;
        m_render = nullptr;
        m_kind = EncoderKind::None;
        m_holdsWork = false;
        m_ran = 0;
        m_kernelPipeline = nullptr;
        m_barriers.reset();
    }
} // namespace rojoRHI::metal4
