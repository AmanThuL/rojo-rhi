//----------------------------------------------------------------------------------------------------------------------
/// @file NativeFrame.h
/// @brief Declares the composed frame's native arm on D3D12: five command lists a frame over two queues and three fences.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/ArmResult.h"
#include "Common/Config.h"
#include "Common/FrameLayout.h"
#include "Common/FrameRoots.h"
#include "Common/Scene.h"
#include "NativeD3D12/NativeContext.h"
#include "NativeD3D12/NativeFrameBarriers.h"
#include "NativeD3D12/NativeFrameResources.h"

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

namespace rojoRHI::bench {
    // The composed frame written natively. Each frame records the cull and ao lists of its slot on the compute queue's own
    // allocators, and the gbuffer, shadows and lighting lists on the context's three direct lists of the slot, then makes
    // the five submissions in order with their fence waits and signals. Each pass writes a PIX event named by its label
    // and a timestamp pair into the slot's query heap, which each list resolves at its end. Each batch of barriers is
    // one enhanced Barrier call (NativeFrameBarriers). Descriptor heaps, root signatures, topology and index buffer are
    // set once per list, and pipeline, viewport and scissor only where they change, since a list keeps them across its
    // render passes. The frame has no CPU span.
    class NativeFrame
    {
      public:
        using Context = NativeContext;
        static constexpr Arm kArm = Arm::Native;
        // The context's direct lists of a slot, in the order the frame uses them.
        static constexpr uint32_t kGbufferList = 0;
        static constexpr uint32_t kShadowsList = 1;
        static constexpr uint32_t kLightingList = 2;
        static constexpr uint32_t kDirectLists = 3;

        NativeFrame(NativeContext& context, const Config& config);
        const std::expected<void, std::string>& ready() const;
        // Records, submits and paces frame `index`, counted from 0; it signals the three fences with index + 1.
        std::expected<FrameResult, std::string> frame(uint32_t index);
        // The outputs of the last frame's slot, in kOutputs order; may be called after any frame.
        std::expected<std::vector<Output>, std::string> readback();

      private:
        // One frame slot's compute lists with their allocators, the query heap of its passes' timestamps and the READBACK
        // buffers its lists resolve them into, one for the graphics queue's lists and one for the compute queue's, so no
        // resource is written by both queues at once.
        struct SlotLists
        {
            NativeList cull;
            NativeList ao;
            ID3D12QueryHeap* timestamps = nullptr;
            std::array<ID3D12Resource*, 2> resolved{};
        };

        std::expected<void, std::string> create();
        std::expected<void, std::string> createLists();
        std::expected<void, std::string> createFences();
        void createBarriers();
        void createRoots();

        void barrier(ID3D12GraphicsCommandList9& list, FrameBatch batch);
        void beginPass(ID3D12GraphicsCommandList9& list, ID3D12QueryHeap* heap, uint32_t pass);
        void endPass(ID3D12GraphicsCommandList9& list, ID3D12QueryHeap* heap, uint32_t pass);
        void openRenderPass(ID3D12GraphicsCommandList9& list, uint32_t pass);
        void bindCompute(ID3D12GraphicsCommandList9& list);
        void bindGraphics(ID3D12GraphicsCommandList9& list);
        HRESULT finish(ID3D12GraphicsCommandList9& list, const SlotLists& lists, FrameList frameList);

        HRESULT recordCull(ID3D12GraphicsCommandList9& list, NativeContext::Slot& slot, const SlotLists& lists, uint32_t slotIndex);
        HRESULT recordGbuffer(ID3D12GraphicsCommandList9& list, NativeContext::Slot& slot, const SlotLists& lists, uint32_t slotIndex);
        HRESULT recordAo(ID3D12GraphicsCommandList9& list, NativeContext::Slot& slot, const SlotLists& lists);
        HRESULT recordShadows(ID3D12GraphicsCommandList9& list, NativeContext::Slot& slot, const SlotLists& lists, uint32_t slotIndex);
        HRESULT recordLighting(ID3D12GraphicsCommandList9& list, NativeContext::Slot& slot, const SlotLists& lists, uint32_t slotIndex);
        HRESULT submit(const SlotLists& lists, const NativeContext::Slot& slot, uint64_t value);

        NativeContext& m_context;
        NativeFrameResources m_resources;
        std::array<SlotLists, kFrameSlots> m_slots{};
        ID3D12Fence* m_cullDone = nullptr;
        ID3D12Fence* m_gbufferDone = nullptr;
        ID3D12Fence* m_aoDone = nullptr;
        std::array<std::array<D3D12_TEXTURE_BARRIER, kMaxBatchTextures>, kFrameBatchCount> m_barriers{};
        std::array<std::wstring, kPassCount> m_passLabels;
        std::array<CullRoot, kFrameSlots> m_cullRoots{};
        GeometryRoot m_geometryRoot{};
        std::array<ShadowRoot, kCascadeCount> m_shadowRoots{};
        AoRoot m_aoRoot{};
        BlurRoot m_blurRoot{};
        LightingRoot m_lightingRoot{};
        std::vector<ForwardRoot> m_forwardRoots; ///< Every forward box's root but its camera, which is the slot's.
        std::vector<uint32_t> m_forwardMeshes;
        BloomDownRoot m_bloomDown0Root{};
        BloomDownRoot m_bloomDown1Root{};
        BloomUpRoot m_bloomUp1Root{};
        BloomUpRoot m_bloomUp0Root{};
        ToneMapRoot m_toneMapRoot{};
        Transform m_camera{};
        std::optional<uint32_t> m_lastIndex;
        std::expected<void, std::string> m_ready;
    };
} // namespace rojoRHI::bench
