//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4PipelineState.h
/// @brief Declares the Metal 4 pipeline payload, the pipeline enum conversions and the depth-stencil state cache.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Counters.h"
#include "Core/Records.h"

#include <Metal/Metal.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <unordered_map>

namespace rojoRHI::metal4 {
    // Everything recording reads to bind it, so a bind reads the payload through `HandlePool::native` and never the
    // record.
    struct PipelineState
    {
        core::PipelineKind kind = core::PipelineKind::Graphics;
        bool mesh = false;
        MTL::RenderPipelineState* render = nullptr;          // +1; null for compute.
        MTL::ComputePipelineState* compute = nullptr;        // +1; null for graphics.
        MTL::DepthStencilState* depthStencil = nullptr;      // Shared through the device cache, not owned; null when nothing is tested.
        MTL::DepthStencilState* boundDepthStencil = nullptr; // Or, when `depthStencil` is null, the device's untested state.
        MTL::CullMode cull = MTL::CullModeNone;
        MTL::Winding winding = MTL::WindingCounterClockwise;
        bool setsWinding = false; // The pipeline culls, tests stencil or has a fragment entry.
        MTL::TriangleFillMode fill = MTL::TriangleFillModeFill;
        MTL::DepthClipMode depthClip = MTL::DepthClipModeClip;    // False clamps.
        bool depthBias = false;                                   // The recorded bias applies.
        MTL::PrimitiveType listType = MTL::PrimitiveTypeTriangle; // The topology a bind resets to.
        MTL::Size objectThreads = MTL::Size(0, 0, 0);
        MTL::Size meshThreads = MTL::Size(0, 0, 0);
        MTL::Size computeThreads = MTL::Size(0, 0, 0);

        // The depth-stencil state belongs to the cache.
        void release();
    };

    MTL::BlendFactor blendFactor(BlendFactor factor);

    MTL::BlendOperation blendOperation(BlendOp op);

    MTL::ColorWriteMask colorWriteMask(ColorWriteMask mask);

    MTL::CompareFunction compareFunction(CompareOp compare);

    MTL::StencilOperation stencilOperation(StencilOp op);

    MTL::CullMode cullMode(CullMode cull);

    MTL::Winding winding(FrontFace face);

    MTL::TriangleFillMode triangleFillMode(FillMode fill);

    MTL::PrimitiveTopologyClass topologyClass(PrimitiveClass primitive);

    MTL::PrimitiveType listType(PrimitiveClass primitive);

    MTL::PrimitiveType primitiveType(PrimitiveTopology topology);

    MTL::IndexType indexType(IndexType type);

    void describeColors(MTL4::RenderPipelineColorAttachmentDescriptorArray* attachments, std::span<const ColorTarget> colors);

    bool testsDepthOrStencil(const DepthStencilState& state);

    // One object per distinct state, so a bind can compare pointers. Free-threaded: creation takes its own mutex.
    class DepthStencilCache
    {
      public:
        DepthStencilCache() = default;
        DepthStencilCache(const DepthStencilCache&) = delete;
        DepthStencilCache& operator=(const DepthStencilCache&) = delete;

        // The untested state equals Metal's default. The caller holds an autorelease pool.
        Result<void> create(MTL::Device* device, core::DeviceCounts& counts);

        MTL::DepthStencilState* untested() const
        {
            return m_untested;
        }

        // Null for a state that tests neither depth nor stencil (Metal's default); null for one that tests something
        // means Metal made no object. The caller holds an autorelease pool.
        MTL::DepthStencilState* get(MTL::Device* device, const DepthStencilState& state, core::DeviceCounts& counts);

        size_t size();

        // Device teardown calls it inside a pool, after every pipeline retired.
        void release(core::DeviceCounts& counts);

      private:
        std::mutex m_mutex;                                             // Guards `m_states`.
        std::unordered_map<uint64_t, MTL::DepthStencilState*> m_states; // By the packed fields Metal receives; each +1.
        MTL::DepthStencilState* m_untested = nullptr;                   // +1; null before `create`.
    };

    // Runs under the pipeline pool's lock.
    void retirePipeline(void* context, core::PipelineRecord& record);
} // namespace rojoRHI::metal4
