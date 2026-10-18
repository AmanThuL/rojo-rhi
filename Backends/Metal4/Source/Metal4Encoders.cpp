//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Encoders.cpp
/// @brief Implements the encoder a Metal 4 command list keeps open: the lazy compute encoder and a clear's render encoders, their opening and closing.
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
    MTL4::ComputeCommandEncoder* Encoders::compute(core::CommandListCore& list, ListState& state, MTL::Stages command)
    {
        ROJORHI_INTERNAL_ASSERT(m_kind != EncoderKind::Render, "a compute command is recorded while a render encoder is open");
        if (m_compute != nullptr)
        {
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
        m_compute->setArgumentTable(state.table);
        core::noteNativeCall(list);
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
        return m_render;
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
            // A render encoder has no stages getter. A clear's render encoder runs only its load and store actions, which are
            // fragment-stage work.
            m_barriers.returnUnrun(MTL::StageFragment);
            m_render->endEncoding();
            core::noteNativeCall(list);
            m_render->release();
            m_render = nullptr;
        }
        m_kind = EncoderKind::None;
        m_holdsWork = false;
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
        m_kernelPipeline = nullptr;
        m_barriers.reset();
    }
} // namespace rojoRHI::metal4
