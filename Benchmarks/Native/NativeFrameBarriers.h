//----------------------------------------------------------------------------------------------------------------------
/// @file NativeFrameBarriers.h
/// @brief Declares the native lowering of every barrier batch of the composed frame: one Metal stage barrier or none per batch.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Native/NativeContext.h"

#include <rojoRHI/RHI.h>

#include <array>
#include <cstdint>

namespace rojoRHI::bench {
    // The barrier batches of the composed frame in recording order: the batch before each pass that has one, the batch
    // inside the `ao` pass, and the batches that close the `gbuffer` and `ao` lists.
    enum class FrameBatch : uint8_t
    {
        Geometry,     ///< Before pass 2, opening the gbuffer list.
        GbufferClose, ///< After pass 2, closing the gbuffer list.
        AoStart,      ///< Before pass 3, opening the ao list.
        AoRaw,        ///< Inside pass 3, between the occlusion and blur dispatches.
        AoClose,      ///< After pass 3, closing the ao list.
        Shadows,      ///< Before pass 4, opening the shadows list.
        Lighting,     ///< Before pass 7, opening the lighting list.
        Forward,      ///< Before pass 8.
        BloomDown0,   ///< Before pass 9.
        BloomDown1,   ///< Before pass 10.
        BloomUp1,     ///< Before pass 11.
        BloomUp0,     ///< Before pass 12.
        ToneMap,      ///< Before pass 13.
        Readback,     ///< Before pass 14.
    };

    // The native form a batch takes: none, a barrier inside the encoder that holds every source, or a consumer barrier
    // on the encoder that runs the destination stages, ordering against earlier encoders and command buffers on its queue.
    enum class NativeBarrierForm : uint8_t
    {
        None,
        Encoder, ///< MTL4::CommandEncoder::barrierAfterEncoderStages.
        Queue,   ///< MTL4::CommandEncoder::barrierAfterQueueStages.
    };

    // One batch: the union of its source stages and of its destination stages as RojoRHI names them, the visibility its
    // accesses select and the form it takes.
    struct NativeBarrier
    {
        FrameBatch batch;
        Stage source;
        Stage destination;
        MTL4::VisibilityOptions visibility;
        NativeBarrierForm form;
    };

    inline constexpr uint32_t kFrameBatchCount = 14;

    /// The Metal stages a RojoRHI stage set lowers to, each bit as the synchronization mapping states it.
    constexpr MTL::Stages metalStages(Stage stages)
    {
        const auto has = [stages](Stage bit) { return (stages & bit) != Stage::None; };
        MTL::Stages result = 0;
        if (stages == Stage::All)
        {
            return MTL::StageAll;
        }
        if (has(Stage::IndirectArgs))
        {
            result |= MTL::StageDispatch | MTL::StageVertex | MTL::StageObject | MTL::StageMesh;
        }
        if (has(Stage::IndexInput) || has(Stage::VertexShader))
        {
            result |= MTL::StageVertex;
        }
        if (has(Stage::TaskShader))
        {
            result |= MTL::StageObject;
        }
        if (has(Stage::MeshShader))
        {
            result |= MTL::StageMesh;
        }
        if (has(Stage::FragmentShader) || has(Stage::ColorAttachment) || has(Stage::DepthStencil) || has(Stage::Resolve))
        {
            result |= MTL::StageFragment;
        }
        if (has(Stage::ComputeShader))
        {
            result |= MTL::StageDispatch;
        }
        // The frame's only copy-stage command is copyTextureToBuffer, a blit-stage copy; it records no kernel fill and no
        // load-action clear, the commands for which the mapping adds the dispatch and fragment stages.
        if (has(Stage::Copy))
        {
            result |= MTL::StageBlit;
        }
        return result;
    }

    // The lowering of every batch, in FrameBatch order; NativeFrameBarriers.cpp states why each takes its form.
    extern const std::array<NativeBarrier, kFrameBatchCount> kFrameBarriers;

    /// The lowering of one batch.
    const NativeBarrier& nativeBarrier(FrameBatch batch);

    /// Records the batch's barrier on `encoder`, which is the encoder its form names, and counts it; a batch of form
    /// None records nothing and alone may pass a null encoder; any other form with one aborts.
    void emitBarrier(MTL4::CommandEncoder* encoder, FrameBatch batch, NativeCalls& calls);
} // namespace rojoRHI::bench
