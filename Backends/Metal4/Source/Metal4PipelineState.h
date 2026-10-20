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
    /// What a Metal 4 pipeline keeps in `PipelineRecord::native`: everything recording reads to bind it, so a bind reads the
    /// payload through `HandlePool::native` and never the record.
    struct PipelineState
    {
        core::PipelineKind kind = core::PipelineKind::Graphics;   ///< Graphics or compute.
        bool mesh = false;                                        ///< A graphics pipeline with a mesh stage.
        MTL::RenderPipelineState* render = nullptr;               ///< The render or mesh state (+1); null for compute.
        MTL::ComputePipelineState* compute = nullptr;             ///< The compute state (+1); null for graphics.
        MTL::DepthStencilState* depthStencil = nullptr;           ///< Shared through the device cache, not owned; null when nothing is tested.
        MTL::DepthStencilState* boundDepthStencil = nullptr;      ///< What a bind sends: `depthStencil`, or when null the device's untested state.
        MTL::CullMode cull = MTL::CullModeNone;                   ///< `RasterState::cull`.
        MTL::Winding winding = MTL::WindingCounterClockwise;      ///< `RasterState::frontFace`.
        bool setsWinding = false;                                 ///< A bind sets the winding: the pipeline culls, tests stencil or has a fragment entry.
        MTL::TriangleFillMode fill = MTL::TriangleFillModeFill;   ///< `RasterState::fill`.
        MTL::DepthClipMode depthClip = MTL::DepthClipModeClip;    ///< `RasterState::depthClip`; false clamps.
        bool depthBias = false;                                   ///< `RasterState::depthBias`: the recorded bias applies.
        MTL::PrimitiveType listType = MTL::PrimitiveTypeTriangle; ///< The list form of the pipeline's class, the topology a bind resets to.
        MTL::Size objectThreads = MTL::Size(0, 0, 0);             ///< The task entry's threadgroup size; zero without one.
        MTL::Size meshThreads = MTL::Size(0, 0, 0);               ///< The mesh entry's threadgroup size; zero without one.
        MTL::Size computeThreads = MTL::Size(0, 0, 0);            ///< The compute entry's threadgroup size; zero for graphics.

        /// Releases the render or compute state; the depth-stencil state belongs to the cache.
        void release();
    };

    /// Returns the Metal blend factor of `factor`.
    MTL::BlendFactor blendFactor(BlendFactor factor);

    /// Returns the Metal blend operation of `op`.
    MTL::BlendOperation blendOperation(BlendOp op);

    /// Returns the Metal write mask of `mask`, whose channel bits are in another order.
    MTL::ColorWriteMask colorWriteMask(ColorWriteMask mask);

    /// Returns the Metal compare function of `compare`.
    MTL::CompareFunction compareFunction(CompareOp compare);

    /// Returns the Metal stencil operation of `op`.
    MTL::StencilOperation stencilOperation(StencilOp op);

    /// Returns the Metal cull mode of `cull`.
    MTL::CullMode cullMode(CullMode cull);

    /// Returns the Metal winding of a front face; counter-clockwise is `MTL::WindingCounterClockwise`.
    MTL::Winding winding(FrontFace face);

    /// Returns the Metal triangle fill mode of `fill`.
    MTL::TriangleFillMode triangleFillMode(FillMode fill);

    /// Returns the Metal topology class of `primitive`.
    MTL::PrimitiveTopologyClass topologyClass(PrimitiveClass primitive);

    /// Returns the list primitive type of `primitive`: points, lines or triangles.
    MTL::PrimitiveType listType(PrimitiveClass primitive);

    /// Returns the Metal primitive type of `topology`.
    MTL::PrimitiveType primitiveType(PrimitiveTopology topology);

    /// Returns the Metal index type of `type`.
    MTL::IndexType indexType(IndexType type);

    /// Describes the color targets `colors` on `attachments`: format, write mask and, when it blends, the factors and
    /// operations.
    void describeColors(MTL4::RenderPipelineColorAttachmentDescriptorArray* attachments, std::span<const ColorTarget> colors);

    /// True when `state` tests depth or stencil, so a pipeline with it needs a depth-stencil state.
    bool testsDepthOrStencil(const DepthStencilState& state);

    /// The device's depth-stencil states, one per distinct state, so equal states share one object and a bind can compare
    /// pointers. Free-threaded: creation takes its own mutex.
    class DepthStencilCache
    {
      public:
        DepthStencilCache() = default;
        DepthStencilCache(const DepthStencilCache&) = delete;
        DepthStencilCache& operator=(const DepthStencilCache&) = delete;

        /// Creates the untested state on `device`: depth compare `Always`, no depth write, no stencil, which is Metal's
        /// default. Adds one to `counts.nativeCreateCalls`. The caller holds an autorelease pool.
        Result<void> create(MTL::Device* device, core::DeviceCounts& counts);

        /// Returns the untested state `create` made; null before it.
        MTL::DepthStencilState* untested() const
        {
            return m_untested;
        }

        /// Returns the state equal to `state`, created on `device` on first use, or null for a state that tests neither depth
        /// nor stencil (Metal's default). Null for a state that tests something means Metal made no object. A creation adds one
        /// to `counts.nativeCreateCalls`. The caller holds an autorelease pool.
        MTL::DepthStencilState* get(MTL::Device* device, const DepthStencilState& state, core::DeviceCounts& counts);

        /// The number of states the cache holds.
        size_t size();

        /// Releases every state, the untested one included, adding one to `counts.nativeCreateCalls` for each. Device teardown
        /// calls it inside a pool, after every pipeline retired.
        void release(core::DeviceCounts& counts);

      private:
        std::mutex m_mutex;                                             ///< Guards `m_states`.
        std::unordered_map<uint64_t, MTL::DepthStencilState*> m_states; ///< By the packed fields Metal receives; each +1.
        MTL::DepthStencilState* m_untested = nullptr;                   ///< The untested state (+1); null before `create`.
    };

    /// Releases the payload of a retiring pipeline and deletes it. `context` is the `DeviceImpl`; a null payload is skipped. It
    /// runs under the pipeline pool's lock.
    void retirePipeline(void* context, core::PipelineRecord& record);
} // namespace rojoRHI::metal4
