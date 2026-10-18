//----------------------------------------------------------------------------------------------------------------------
/// @file NativeFrameBarriers.cpp
/// @brief Defines the native lowering of the composed frame's barrier batches and records them.
//----------------------------------------------------------------------------------------------------------------------
#include "Native/NativeFrameBarriers.h"

#include <cstdio>
#include <cstdlib>

namespace rojoRHI::bench {
    namespace {
        constexpr MTL4::VisibilityOptions kDevice = MTL4::VisibilityOptionDevice;
        constexpr MTL4::VisibilityOptions kNone = MTL4::VisibilityOptionNone;
    } // namespace

    // A batch lowers to one stage barrier from the union of its source stages to the union of its destination stages,
    // with device visibility when it names any access, and to none when its source stage is None. It is encoder-level
    // when every source since the last queue-level barrier lies in the encoder open when it is recorded; otherwise it is
    // the consumer barrierAfterQueueStages on the next encoder that runs its destination stages. Each list opens a compute
    // encoder at its first dispatch or copy, a render encoder per render pass, and ends the open encoder at the end of
    // every pass, because a pass's closing timestamp and debug group are written on the command buffer. Layouts have no
    // native effect.
    const std::array<NativeBarrier, kFrameBatchCount> kFrameBarriers{ {
        // albedo, normal, material: ComputeShader/None -> ColorAttachment/ColorAttachmentWrite; depth: DepthStencil/None ->
        // DepthStencil/DepthStencilRead+DepthStencilWrite, since the pass tests and writes depth. Both depth accesses select
        // device visibility, as the write alone would. It opens the list, so no encoder holds a source: queue-level. By the
        // backend design's pending-barrier rule (M7), each encoder opened while it is pending emits it toward the
        // destination stages it runs: the pass's render encoder emits all of it, and the counted draw's kernel encoder,
        // opened first, runs no fragment stage and emits none.
        { FrameBatch::Geometry, Stage::ComputeShader | Stage::DepthStencil, Stage::ColorAttachment | Stage::DepthStencil, kDevice, NativeBarrierForm::Queue },
        // albedo, normal, material: ColorAttachment/ColorAttachmentWrite -> ComputeShader/ShaderRead; depth: DepthStencil/
        // DepthStencilWrite -> ComputeShader/ShaderRead. The render encoder ended with the pass, and no encoder follows in
        // the list, so the queue-level barrier has no encoder to emit it: none. The ao list on the compute queue reads
        // these targets behind the wait on gbufferDone, which the gbuffer list's submission signals on completion.
        { FrameBatch::GbufferClose, Stage::ColorAttachment | Stage::DepthStencil, Stage::ComputeShader, kDevice, NativeBarrierForm::None },
        // aoRaw, ao: Undefined -> ShaderWrite, None/None -> ComputeShader/ShaderWrite. No source stage: none.
        { FrameBatch::AoStart, Stage::None, Stage::ComputeShader, kNone, NativeBarrierForm::None },
        // aoRaw: ComputeShader/ShaderWrite -> ComputeShader/ShaderRead, between the occlusion and blur dispatches. Its source,
        // the occlusion dispatch, lies in the open compute encoder: encoder-level.
        { FrameBatch::AoRaw, Stage::ComputeShader, Stage::ComputeShader, kDevice, NativeBarrierForm::Encoder },
        // ao: ComputeShader/ShaderWrite -> ComputeShader/ShaderRead. The compute encoder ended with the pass and no encoder
        // follows in the list: none. The lighting list reads ao behind the wait on aoDone.
        { FrameBatch::AoClose, Stage::ComputeShader, Stage::ComputeShader, kDevice, NativeBarrierForm::None },
        // shadow: Undefined -> DepthStencilWrite, ComputeShader/None -> DepthStencil/DepthStencilRead+DepthStencilWrite,
        // since the cascades test and write depth; either access selects device visibility. It opens the list: queue-level,
        // emitted by the render encoder of shadow0, not by the kernel encoder before it, which runs no fragment stage (the
        // backend design's pending-barrier rule, M7).
        { FrameBatch::Shadows, Stage::ComputeShader, Stage::DepthStencil, kDevice, NativeBarrierForm::Queue },
        // shadow: DepthStencil/DepthStencilWrite -> ComputeShader/ShaderRead; hdr: FragmentShader/None -> ComputeShader/
        // ShaderWrite. It opens the list; its sources lie in the shadows list and the previous frame: queue-level, emitted
        // by the lighting pass's compute encoder.
        { FrameBatch::Lighting, Stage::DepthStencil | Stage::FragmentShader, Stage::ComputeShader, kDevice, NativeBarrierForm::Queue },
        // hdr: ComputeShader/ShaderWrite -> ColorAttachment/ColorAttachmentRead+Write; depth: ComputeShader/None ->
        // DepthStencil/DepthStencilRead. The lighting encoder ended with its pass: queue-level, on the forward encoder.
        { FrameBatch::Forward, Stage::ComputeShader, Stage::ColorAttachment | Stage::DepthStencil, kDevice, NativeBarrierForm::Queue },
        // hdr: ColorAttachment/ColorAttachmentWrite -> FragmentShader/ShaderRead; bloom0: FragmentShader/None ->
        // ColorAttachment/ColorAttachmentWrite. The forward encoder ended with its pass: queue-level, on bloomDown0's encoder.
        { FrameBatch::BloomDown0,
          Stage::ColorAttachment | Stage::FragmentShader,
          Stage::FragmentShader | Stage::ColorAttachment,
          kDevice,
          NativeBarrierForm::Queue },
        // bloom0 -> ShaderRead, bloom1 -> ColorAttachment, as before pass 9: queue-level, on bloomDown1's encoder.
        { FrameBatch::BloomDown1,
          Stage::ColorAttachment | Stage::FragmentShader,
          Stage::FragmentShader | Stage::ColorAttachment,
          kDevice,
          NativeBarrierForm::Queue },
        // bloom1: ColorAttachment/ColorAttachmentWrite -> FragmentShader/ShaderRead; bloom0: FragmentShader/None ->
        // ColorAttachment/ColorAttachmentRead+Write: queue-level, on bloomUp1's encoder.
        { FrameBatch::BloomUp1,
          Stage::ColorAttachment | Stage::FragmentShader,
          Stage::FragmentShader | Stage::ColorAttachment,
          kDevice,
          NativeBarrierForm::Queue },
        // bloom0 -> ShaderRead, hdr -> ColorAttachment, as before pass 11: queue-level, on bloomUp0's encoder.
        { FrameBatch::BloomUp0,
          Stage::ColorAttachment | Stage::FragmentShader,
          Stage::FragmentShader | Stage::ColorAttachment,
          kDevice,
          NativeBarrierForm::Queue },
        // hdr: ColorAttachment/ColorAttachmentWrite -> FragmentShader/ShaderRead; ldr: Copy/None -> ColorAttachment/
        // ColorAttachmentWrite, the previous frame's copy of ldr: queue-level, on the tone map's encoder.
        { FrameBatch::ToneMap, Stage::ColorAttachment | Stage::Copy, Stage::FragmentShader | Stage::ColorAttachment, kDevice, NativeBarrierForm::Queue },
        // ldr: ColorAttachment/ColorAttachmentWrite -> Copy/CopyRead. The tone map's encoder ended with its pass:
        // queue-level, on the compute encoder that runs the copy.
        { FrameBatch::Readback, Stage::ColorAttachment, Stage::Copy, kDevice, NativeBarrierForm::Queue },
    } };

    //==================================================================================================================
    const NativeBarrier& nativeBarrier(FrameBatch batch)
    {
        const NativeBarrier& barrier = kFrameBarriers[static_cast<size_t>(batch)];
        if (barrier.batch != batch)
        {
            std::fprintf(stderr, "native frame barrier table out of order at %u\n", static_cast<unsigned>(batch));
            std::abort();
        }
        return barrier;
    }

    //==================================================================================================================
    void emitBarrier(MTL4::CommandEncoder* encoder, FrameBatch batch, NativeCalls& calls)
    {
        const NativeBarrier& barrier = nativeBarrier(batch);
        // Only a batch that makes no barrier is recorded without an encoder.
        if (encoder == nullptr && barrier.form != NativeBarrierForm::None)
        {
            std::fprintf(stderr, "native frame barrier %u recorded without an encoder\n", static_cast<unsigned>(batch));
            std::abort();
        }
        switch (barrier.form)
        {
        case NativeBarrierForm::None: return;
        case NativeBarrierForm::Encoder:
            encoder->barrierAfterEncoderStages(metalStages(barrier.source), metalStages(barrier.destination), barrier.visibility);
            break;
        case NativeBarrierForm::Queue:
            encoder->barrierAfterQueueStages(metalStages(barrier.source), metalStages(barrier.destination), barrier.visibility);
            break;
        }
        ++calls.record;
        ++calls.barriers;
    }
} // namespace rojoRHI::bench
