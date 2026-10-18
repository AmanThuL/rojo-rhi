//----------------------------------------------------------------------------------------------------------------------
/// @file RojoFrame.h
/// @brief Declares the composed frame's RojoRHI arm: five lists of fourteen passes on two queues, fenced per frame.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/ArmResult.h"
#include "Common/Config.h"
#include "Common/FrameLayout.h"
#include "Common/FrameRoots.h"
#include "Common/Scene.h"
#include "Rojo/RojoCalls.h"
#include "Rojo/RojoContext.h"
#include "Rojo/RojoFrameResources.h"

#include <rojoRHI/RHI.h>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace rojoRHI::bench {
    // The composed frame as the pre-registration's pass table records it through RHI.h: per frame, the five lists of
    // FrameLayout acquired in order, each pass's barrier batch as one barrier call before it, the two closing batches,
    // and five submissions waiting on and signalling cullDone, gbufferDone and aoDone with the frame's number, counted
    // from 1. Every list records through bench::List, so Null builds trace it.
    class RojoFrame
    {
      public:
        using Context = RojoContext;
        static constexpr Arm kArm = Arm::Rojo;

        RojoFrame(RojoContext& context, const Config& config);
        const std::expected<void, std::string>& ready() const;
        std::expected<FrameResult, std::string> frame(uint32_t index);
        // One frame after the measured ones that copies every texture and records[s] of the last frame's slot into
        // Readback buffers, then the outputs of FrameLayout's kOutputs in order.
        std::expected<std::vector<Output>, std::string> readback();

      private:
        std::expected<void, std::string> create();

        void recordCull(List& list, uint32_t slot);
        void recordGbuffer(List& list, uint32_t slot);
        void recordAo(List& list);
        void recordShadows(List& list, uint32_t slot);
        void recordLighting(List& list, uint32_t slot);
        // Begins the render pass of pass `pass` of kPasses and sets its pipeline, viewport and scissor.
        void beginPass(List& list, uint32_t pass, Pipeline pipeline);
        // Records one full-screen pass of BenchPost: its barrier batch, the pass, its root and one draw of 3 vertices.
        template<class Root> void fullScreenPass(List& list, uint32_t pass, Pipeline pipeline, std::span<const TextureBarrier> batch, const Root& root);

        RojoContext& m_context;
        RojoFrameResources m_resources;
        Fence m_cullDone;
        Fence m_gbufferDone;
        Fence m_aoDone;

        // The roots, fixed at creation: every value is the scene's or a view, sampler or address of the resources. A
        // forward root's camera is the frame's upload, so it is set while recording.
        std::array<CullRoot, kFrameSlots> m_cullRoots{};
        GeometryRoot m_geometryRoot{};
        std::array<ShadowRoot, kCascadeCount> m_shadowRoots{};
        AoRoot m_aoRoot{};
        BlurRoot m_blurRoot{};
        LightingRoot m_lightingRoot{};
        std::vector<ForwardRoot> m_forwardRoots;
        std::vector<uint32_t> m_forwardMeshes;
        BloomDownRoot m_bloomDown0Root{};
        BloomDownRoot m_bloomDown1Root{};
        BloomUpRoot m_bloomUp1Root{};
        BloomUpRoot m_bloomUp0Root{};
        ToneMapRoot m_toneMapRoot{};
        Transform m_camera{};
        std::array<Addr, kFrameSlots> m_records{};
        uint32_t m_lastFrame = 0;
        std::expected<void, std::string> m_ready;
    };
} // namespace rojoRHI::bench
