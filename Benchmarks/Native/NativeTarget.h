//----------------------------------------------------------------------------------------------------------------------
/// @file NativeTarget.h
/// @brief Declares the native arm's render targets of the drawing CPU workloads, their passes, pipelines and readback.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/ArmResult.h"
#include "Common/CpuRoots.h"
#include "Native/NativeContext.h"

#include <Metal/Metal.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::bench {
    // Per frame slot, a 256x256 RGBA8Unorm color texture and, when asked, a D32Float depth texture, both Private, and a
    // render pass descriptor that clears color to zero and depth to 0 and stores both. Frame f renders to slot f mod 3
    // only, as the RojoRHI arm does.
    class NativeTarget
    {
      public:
        // The whole target, set once per frame before the repetitions.
        static constexpr MTL::Viewport kViewport{ 0.0, 0.0, double{ kTargetSize }, double{ kTargetSize }, 0.0, 1.0 };
        static constexpr MTL::ScissorRect kScissor{ 0, 0, kTargetSize, kTargetSize };

        // Creates the textures and descriptors through the context, before prepareFrames commits the residency set.
        std::expected<void, std::string> create(NativeContext& context, std::string_view label, bool depth);

        MTL4::RenderPassDescriptor* pass(uint32_t slot) const;

        // A pipeline drawing into the color target with a vertex and a fragment function of `library`.
        static std::expected<MTL::RenderPipelineState*, std::string> pipeline(
            NativeContext& context, MTL::Library* library, std::string_view vertex, std::string_view fragment, std::string_view label
        );

        // Outside every measurement: "target", then "depth" when the slot has one, copied into a Shared buffer by one
        // frame numbered `frameIndex`, which follows every frame the workload ran.
        std::expected<std::vector<Output>, std::string> read(NativeContext& context, uint32_t frameIndex, uint32_t slot) const;

      private:
        std::array<MTL::Texture*, NativeContext::kFrameSlots> m_color{};
        std::array<MTL::Texture*, NativeContext::kFrameSlots> m_depth{};
        std::array<MTL4::RenderPassDescriptor*, NativeContext::kFrameSlots> m_passes{};
    };
} // namespace rojoRHI::bench
