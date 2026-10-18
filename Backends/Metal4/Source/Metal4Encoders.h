//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Encoders.h
/// @brief Declares the encoder a Metal 4 command list keeps open: the compute encoder that opens lazily for copies and fills, and a clear's render encoders.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Core/CommandLists.h"
#include "Metal4Barriers.h"
#include "Metal4ListState.h"

#include <Metal/Metal.hpp>

#include <cstdint>

namespace rojoRHI::metal4 {
    /// The kind of encoder a list has open.
    enum class EncoderKind : uint8_t
    {
        None,    ///< No encoder is open.
        Compute, ///< The compute encoder, which records copies, fills and backend kernels.
        Render,  ///< A render encoder, which clears one subresource by its load actions.
    };

    /// The one encoder a command list may have open. The compute encoder opens on the first command that needs it and stays
    /// open until a scope boundary, a queue-level barrier, a clear's render encoder or `CommandList::end` closes it, so
    /// consecutive copies and fills share it. Only the list's recording thread touches it.
    class Encoders
    {
      public:
        /// Returns the open compute encoder for a command about to be recorded in stage `command`, first emitting the part of
        /// the encoder-level barrier held back for that stage, or opens one when none is open: the factory runs inside an
        /// autorelease pool and the encoder is retained; it takes the innermost open scope's label, else the list's, and the
        /// list's argument table, then emits the pending barrier's part on its stages. The opening counts its native calls,
        /// its barrier and one encoder on `list`. The new encoder holds no work.
        MTL4::ComputeCommandEncoder* compute(core::CommandListCore& list, ListState& state, MTL::Stages command);

        /// Opens a render encoder from `descriptor` when no encoder is open: the factory runs inside an autorelease pool and the
        /// encoder is retained; it takes the innermost open scope's label, else the list's, and binds no table, because no
        /// shader runs on it; then it emits the pending barrier's part on the fragment stage, where its load and store actions
        /// run, and leaves the other render stages pending. The opening counts its native calls, its barrier and one encoder
        /// on `list`. The caller closes it with `close`.
        MTL4::RenderCommandEncoder* openRender(core::CommandListCore& list, ListState& state, const MTL4::RenderPassDescriptor* descriptor);

        /// Notes that the open encoder holds work.
        void markWork()
        {
            m_holdsWork = true;
        }

        /// Returns true when an encoder is open and holds work.
        bool holdsWork() const
        {
            return m_holdsWork;
        }

        /// Returns the kind of the open encoder.
        EncoderKind kind() const
        {
            return m_kind;
        }

        /// Returns the open compute encoder, or null when none is open.
        MTL4::ComputeCommandEncoder* computeEncoder() const
        {
            return m_compute;
        }

        /// Returns the stages of the open encoder; none when no encoder is open.
        MTL::Stages stages() const
        {
            switch (m_kind)
            {
            case EncoderKind::Compute: return kComputeStages;
            case EncoderKind::Render: return kRenderStages;
            case EncoderKind::None: break;
            }
            return 0;
        }

        /// Returns the backend kernel pipeline set on the open compute encoder, or null when none is set.
        const MTL::ComputePipelineState* kernelPipeline() const
        {
            return m_kernelPipeline;
        }

        /// Records that `pipeline` is set on the open compute encoder.
        void setKernelPipeline(const MTL::ComputePipelineState* pipeline)
        {
            m_kernelPipeline = pipeline;
        }

        /// Returns what the list owes of its barriers and what the open encoder's opening barrier covered.
        BarrierState& barriers()
        {
            return m_barriers;
        }

        /// Returns what the list owes of its barriers and what the open encoder's opening barrier covered.
        const BarrierState& barriers() const
        {
            return m_barriers;
        }

        /// Ends the open encoder, counting its `endEncoding` on `list`, and releases it; an encoder-level barrier still held
        /// back joins the pending barrier, as do the stages its opening barrier held back in which it ran no command; what that
        /// barrier covered, and the kernel pipeline set on it, are then forgotten. Does nothing when none is open.
        void close(core::CommandListCore& list);

        /// Releases an encoder still open without ending it and forgets every barrier: a list that never ended, which only a
        /// Release build lets through, leaves them behind. It opens an autorelease pool only when there is one to release.
        void release();

      private:
        MTL4::ComputeCommandEncoder* m_compute = nullptr; ///< The open compute encoder, retained; null when none is open.
        MTL4::RenderCommandEncoder* m_render = nullptr;   ///< The open render encoder, retained; null when none is open.
        EncoderKind m_kind = EncoderKind::None;           ///< The kind of the open encoder.
        bool m_holdsWork = false;                         ///< The open encoder holds at least one command.
        BarrierState m_barriers;                          ///< The pending barrier and the open encoder's covered stages.
        /// The backend kernel pipeline last set on the open compute encoder, so a kernel skips setting it again, which the
        /// validation layer reports as redundant. Reset when an encoder opens or closes. Any entry that sets another compute
        /// pipeline on the encoder, such as `CommandList::setPipeline`, must clear it.
        const MTL::ComputePipelineState* m_kernelPipeline = nullptr;
    };
} // namespace rojoRHI::metal4
