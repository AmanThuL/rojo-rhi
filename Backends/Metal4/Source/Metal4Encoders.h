//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Encoders.h
/// @brief Declares the encoder a Metal 4 command list keeps open: the lazy compute encoder, a clear's render encoders and a render pass's.
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
        Render,  ///< A render encoder: a clear's, which clears one subresource by its load actions, or a render pass's.
    };

    /// The one encoder a command list may have open. The compute encoder opens on the first command that needs it and stays
    /// open until a scope boundary, a queue-level barrier, a clear's render encoder, a render pass or `CommandList::end`
    /// closes it, so consecutive copies and fills share it. Only the list's recording thread touches it.
    class Encoders
    {
      public:
        /// Returns the open compute encoder for a command about to be recorded in stage `command`, first emitting the part of
        /// the encoder-level barrier held back for that stage (and, while it holds no work, the part of the pending barrier on
        /// its stages, which a barrier recorded after it opened left there), or opens one when none is open: the factory runs
        /// inside an autorelease pool and the encoder is retained; it takes the innermost open scope's label, else the list's,
        /// and the list's argument table, unless `kernelsOnly`, then emits the pending barrier's part on its stages. The opening
        /// counts its native calls, its barrier and one encoder on `list`. The new encoder holds no work. An encoder opened
        /// `kernelsOnly` runs only backend kernels, which bind the kernel table (`bindKernelBatch`): a render pass's counted draws
        /// open it ahead of the render encoder, and nothing else records on a compute encoder inside a pass (chapter 07).
        MTL4::ComputeCommandEncoder* compute(core::CommandListCore& list, ListState& state, MTL::Stages command, bool kernelsOnly = false);

        /// Opens a render encoder from `descriptor` when no encoder is open: the factory runs inside an autorelease pool and the
        /// encoder is retained; it takes the innermost open scope's label, else the list's, and binds no table, because no
        /// shader runs on it; then it emits the pending barrier's part on the fragment stage, where its load and store actions
        /// run, and leaves the other render stages pending. The opening counts its native calls, its barrier and one encoder
        /// on `list`. The caller closes it with `close`.
        MTL4::RenderCommandEncoder* openRender(core::CommandListCore& list, ListState& state, const MTL4::RenderPassDescriptor* descriptor);

        /// Opens a render pass's encoder from `descriptor` when no encoder is open: the factory runs inside an autorelease pool
        /// and the encoder is retained; it takes the innermost open scope's label, else the list's, and binds the list's argument
        /// table to the vertex, fragment, object and mesh stages (ADR 0018), then emits the pending barrier's part on every
        /// render stage. The opening counts its native calls, its barrier and one encoder on `list`. The encoder has run its
        /// fragment stage, where its load and store actions run; `markRan` adds the stages its commands run in.
        MTL4::RenderCommandEncoder* openPass(core::CommandListCore& list, ListState& state, const MTL4::RenderPassDescriptor* descriptor);

        /// Notes that the open render encoder ran a command in `stages`, so its opening barrier's hold on them is spent.
        void markRan(MTL::Stages stages)
        {
            m_ran |= stages;
        }

        /// Records the carrier of an upscale when no encoder is open: a compute encoder that waits on the fence an earlier
        /// upscale left owed to either encoder kind and then for every stage of all earlier work on the queue, which covers the
        /// pending barrier's part on its stages, then updates `fence`
        /// after those stages and ends. MetalFX's encoders wait on `fence`, so the barriers before the upscale reach them
        /// through it. The encoder takes the innermost open scope's label, else the list's, binds no table, because no shader
        /// runs on it, and is used and ended inside the autorelease pool its factory needs. Counts its native calls, its
        /// barrier and one encoder on `list`.
        void carry(core::CommandListCore& list, ListState& state, MTL::Fence* fence);

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

        /// Returns the open render encoder, or null when none is open.
        MTL4::RenderCommandEncoder* renderEncoder() const
        {
            return m_render;
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

        /// Returns the compute pipeline set on the open compute encoder, a backend kernel's or the caller's, or null when none
        /// is set.
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
        /// back joins the pending barrier, as do the stages its opening barrier held back in which it ran no command (for a
        /// render encoder, those `markRan` did not name besides the fragment stage); what that barrier covered, and the kernel
        /// pipeline set on it, are then forgotten. A render encoder ends inside an autorelease pool, because the validation
        /// layer autoreleases there. Does nothing when none is open.
        void close(core::CommandListCore& list);

        /// Releases an encoder still open without ending it and forgets every barrier: a list that never ended, which only a
        /// Release build lets through, leaves them behind. It opens an autorelease pool only when there is one to release.
        void release();

      private:
        MTL4::ComputeCommandEncoder* m_compute = nullptr; ///< The open compute encoder, retained; null when none is open.
        MTL4::RenderCommandEncoder* m_render = nullptr;   ///< The open render encoder, retained; null when none is open.
        EncoderKind m_kind = EncoderKind::None;           ///< The kind of the open encoder.
        bool m_holdsWork = false;                         ///< The open encoder holds at least one command.
        MTL::Stages m_ran = 0;                            ///< The stages the open render encoder ran a command in.
        BarrierState m_barriers;                          ///< The pending barrier and the open encoder's covered stages.
        /// The compute pipeline last set on the open compute encoder, a backend kernel's or the caller's, so neither is set
        /// again while the encoder holds it, which the validation layer reports as redundant. Reset when an encoder opens or
        /// closes. Every entry that sets a compute pipeline on the encoder records it with `setKernelPipeline`.
        const MTL::ComputePipelineState* m_kernelPipeline = nullptr;
    };
} // namespace rojoRHI::metal4
