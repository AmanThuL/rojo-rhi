//----------------------------------------------------------------------------------------------------------------------
/// @file NativeFrame.h
/// @brief Declares the composed frame's native arm: five metal-cpp command buffers a frame over two queues and three events.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/ArmResult.h"
#include "Common/Config.h"
#include "Common/FrameLayout.h"
#include "Native/NativeContext.h"
#include "Native/NativeFrameResources.h"
#include "Native/NativeIndirect.h"

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace rojoRHI::bench {
    // The composed frame written natively. Each frame records the cull, gbuffer, ao, shadows and lighting command buffers
    // of its slot, each from its own labelled allocator, then makes the five submissions in order with their event waits
    // and signals; every commit carries feedback, and the frame's GPU span runs from the earliest start of its five commits
    // to the latest end. Each pass pushes a debug group named by its label, writes a timestamp pair into its list's counter
    // heap and labels its encoders; the heaps of a slot are resolved when the slot is next used. Barriers are lowered by
    // NativeFrameBarriers. The frame has no CPU span.
    class NativeFrame
    {
      public:
        using Context = NativeContext;
        static constexpr Arm kArm = Arm::Native;

        NativeFrame(NativeContext& context, const Config& config);
        const std::expected<void, std::string>& ready() const;
        // Records, submits and waits frame `index`, counted from 0; it signals the events with index + 1.
        std::expected<FrameResult, std::string> frame(uint32_t index);
        // The outputs of the last frame's slot, in kOutputs order; may be called after any frame.
        std::expected<std::vector<Output>, std::string> readback();

      private:
        // One frame slot's command buffers, allocators, counter heaps and argument table, indexed by FrameList.
        struct SlotLists
        {
            std::array<MTL4::CommandAllocator*, kListCount> allocators{};
            std::array<MTL4::CommandBuffer*, kListCount> lists{};
            std::array<MTL4::CounterHeap*, kListCount> heaps{};
            MTL4::ArgumentTable* table = nullptr;
            bool written = false; ///< The heaps hold timestamps of a completed frame.
        };

        // The compute pipelines and the render pipelines.
        struct Pipelines
        {
            MTL::ComputePipelineState* cull = nullptr;
            MTL::ComputePipelineState* ao = nullptr;
            MTL::ComputePipelineState* blur = nullptr;
            MTL::ComputePipelineState* lighting = nullptr;
            MTL::RenderPipelineState* geometry = nullptr;
            MTL::RenderPipelineState* shadow = nullptr;
            MTL::RenderPipelineState* forward = nullptr;
            MTL::RenderPipelineState* bloomDown = nullptr;
            MTL::RenderPipelineState* bloomUp = nullptr;
            MTL::RenderPipelineState* toneMap = nullptr;
        };

        std::expected<void, std::string> create();
        std::expected<void, std::string> createPipelines();
        std::expected<void, std::string> createPasses();
        std::expected<void, std::string> createCountedDraws();
        std::expected<void, std::string> createLists();
        std::expected<void, std::string> createEvents();

        void resolve(SlotLists& lists);
        void beginPass(MTL4::CommandBuffer* commands, MTL4::CounterHeap* heap, uint32_t pass, uint32_t entry);
        void endPass(MTL4::CommandBuffer* commands, MTL4::CounterHeap* heap, uint32_t entry);
        MTL4::RenderCommandEncoder* openRenderPass(MTL4::CommandBuffer* commands, uint32_t pass);
        void setRenderState(MTL4::RenderCommandEncoder* encoder, uint32_t pass, MTL::RenderPipelineState* pipeline, MTL4::ArgumentTable* table);
        void drawFullscreen(MTL4::RenderCommandEncoder* encoder, MTL4::ArgumentTable* table, uint64_t root);

        void recordCull(SlotLists& lists, NativeContext::Slot& slot, uint32_t slotIndex);
        void recordGbuffer(SlotLists& lists, NativeContext::Slot& slot, uint32_t slotIndex);
        void recordAo(SlotLists& lists, NativeContext::Slot& slot);
        void recordShadows(SlotLists& lists, NativeContext::Slot& slot, uint32_t slotIndex);
        void recordLighting(SlotLists& lists, NativeContext::Slot& slot, uint32_t slotIndex);
        void submit(const SlotLists& lists, uint64_t value);

        NativeContext& m_context;
        NativeFrameResources m_resources;
        NativeIndirect m_indirect;
        Pipelines m_pipelines;
        MTL::DepthStencilState* m_depthWrite = nullptr;
        MTL::DepthStencilState* m_depthTestOnly = nullptr;
        std::array<MTL4::RenderPassDescriptor*, kRenderPassCount> m_renderPasses{};
        std::array<NS::String*, kPassCount> m_passLabels{};
        std::array<SlotLists, kFrameSlots> m_slots{};
        MTL::SharedEvent* m_cullDone = nullptr;
        MTL::SharedEvent* m_gbufferDone = nullptr;
        MTL::SharedEvent* m_aoDone = nullptr;
        std::optional<uint32_t> m_lastIndex;
        std::expected<void, std::string> m_ready;
    };
} // namespace rojoRHI::bench
