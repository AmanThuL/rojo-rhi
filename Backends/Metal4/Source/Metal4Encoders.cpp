//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Encoders.cpp
/// @brief Implements the encoder a Metal 4 command list keeps open: the lazy compute encoder's opening and closing.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Encoders.h"

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
        return m_compute;
    }

    //==================================================================================================================
    void Encoders::close(core::CommandListCore& list)
    {
        if (m_compute == nullptr)
        {
            return;
        }
        // An encoder-level barrier still held back orders nothing in later encoders, so it waits for them at queue level.
        m_barriers.queueDeferred();
        // stages() reads the stages the encoder's commands ran in; it records nothing.
        m_barriers.returnUnrun(m_compute->stages());
        m_compute->endEncoding();
        core::noteNativeCall(list);
        m_compute->release();
        m_compute = nullptr;
        m_kind = EncoderKind::None;
        m_holdsWork = false;
        m_barriers.clearOpen();
    }

    //==================================================================================================================
    void Encoders::release()
    {
        if (m_compute != nullptr)
        {
            const AutoreleasePool pool;
            m_compute->release();
        }
        m_compute = nullptr;
        m_kind = EncoderKind::None;
        m_holdsWork = false;
        m_barriers.reset();
    }
} // namespace rojoRHI::metal4
