//----------------------------------------------------------------------------------------------------------------------
/// @file RojoTarget.h
/// @brief Declares the RojoRHI arm's render targets of the drawing CPU workloads, their passes, pipelines and readback.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/ArmResult.h"
#include "Common/CpuRoots.h"
#include "Rojo/RojoContext.h"

#include <rojoRHI/RHI.h>

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::bench {
    // Per frame slot, a 256x256 RGBA8Unorm color target and, when asked, a D32Float depth target, and a render pass that
    // clears color to zero and depth to 0 and stores both. Frame f renders to slot f mod 3 only: the validation tracker
    // orders a frame's attachment writes after those of the frame three earlier once beginFrame retires that slot,
    // where a single target would need a barrier every frame (VU-rojo-sync-0021).
    class RojoTarget
    {
      public:
        static constexpr uint32_t kSlots = 3;
        // The whole target, set once per frame before the repetitions.
        static constexpr Viewport kViewport{ .width = float{ kTargetSize }, .height = float{ kTargetSize } };
        static constexpr Rect2D kScissor{ .width = kTargetSize, .height = kTargetSize };

        // Creates the textures and moves them into their attachment layouts in one setup frame.
        std::expected<void, std::string> create(RojoContext& context, std::string_view label, bool depth);

        // The pass over slot `slot`'s targets; its label names the pass's scope.
        RenderPassDesc pass(uint32_t slot) const;

        Texture color(uint32_t slot) const;

        // A pipeline drawing into the color target, and into the depth target when the target has one.
        std::expected<Pipeline, std::string> pipeline(
            RojoContext& context,
            ShaderLibrary library,
            std::string_view vertex,
            std::string_view fragment,
            const RasterState& raster,
            const DepthStencilState& depthStencil,
            std::string_view label
        ) const;

        // Outside every measurement: "target", then "depth" when the slot has one, copied into a Readback buffer by one
        // frame that moves them into CopySrc and signals a fence the CPU waits on.
        std::expected<std::vector<Output>, std::string> read(RojoContext& context, uint32_t slot);

      private:
        std::array<Texture, kSlots> m_color{};
        std::array<Texture, kSlots> m_depth{};
        std::array<std::array<ColorAttachment, 1>, kSlots> m_attachments{};
        std::string m_label;
        bool m_hasDepth = false;
        Fence m_readbackFence;
        uint64_t m_readbackValue = 0;
    };
} // namespace rojoRHI::bench
