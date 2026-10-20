//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12PipelineState.h
/// @brief Declares the D3D12 pipeline payload, the pipeline state conversions and the pipeline state streams pipelines are built from.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/Records.h"
#include "D3D12Common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace rojoRHI::d3d12 {
    /// What a pipeline draws or dispatches with: a vertex stage, task and mesh stages, or a compute stage.
    enum class PipelineKind : uint8_t
    {
        Graphics, ///< A vertex stage and an optional fragment stage.
        Mesh,     ///< An optional task stage, a mesh stage and an optional fragment stage.
        Compute,  ///< A compute stage.
    };

    /// What a D3D12 pipeline keeps in `PipelineRecord::native`: everything recording reads to bind it, so a bind reads the
    /// payload through `HandlePool::native` and never the record. Nothing in it changes while the pipeline lives.
    struct PipelineState
    {
        ID3D12PipelineState* pso = nullptr;                                     ///< The native pipeline, one reference owned.
        PipelineKind kind = PipelineKind::Graphics;                             ///< The stages the pipeline has.
        D3D_PRIMITIVE_TOPOLOGY listTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED; ///< The list form of a vertex pipeline's class; undefined otherwise.
        bool depthBias = false;                                                 ///< `RasterState::depthBias`: the recorded bias applies.
    };

    /// Returns the D3D12 blend factor of `factor` in a colour slot.
    D3D12_BLEND colorBlend(BlendFactor factor);

    /// Returns the D3D12 blend factor of `factor` in an alpha slot, which takes no colour factor: a colour factor becomes
    /// its alpha form, and a constant reads the blend factor's alpha.
    D3D12_BLEND alphaBlend(BlendFactor factor);

    /// Returns the D3D12 blend operation of `op`.
    D3D12_BLEND_OP blendOp(BlendOp op);

    /// Returns the `D3D12_COLOR_WRITE_ENABLE` bits of `mask`.
    UINT8 writeMask(ColorWriteMask mask);

    /// Returns the D3D12 cull mode of `cull`.
    D3D12_CULL_MODE cullMode(CullMode cull);

    /// Returns the D3D12 fill mode of `fill`.
    D3D12_FILL_MODE fillMode(FillMode fill);

    /// Returns the D3D12 comparison function of `compare`.
    D3D12_COMPARISON_FUNC comparisonFunc(CompareOp compare);

    /// Returns the D3D12 stencil operation of `op`.
    D3D12_STENCIL_OP stencilOp(StencilOp op);

    /// Returns the topology type a pipeline of `primitive` class is created with.
    D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType(PrimitiveClass primitive);

    /// Returns the list topology of `primitive` class: a point, line or triangle list.
    D3D_PRIMITIVE_TOPOLOGY listTopology(PrimitiveClass primitive);

    /// Returns the blend state of `colors`, one render target each, independent blending on and alpha to coverage off; a
    /// render target past the last colour keeps the default state.
    D3D12_BLEND_DESC blendDesc(std::span<const ColorTarget> colors);

    /// Returns the rasterizer state of `raster`, its bias fields zero: a pipeline with `depthBias` takes the bias from the
    /// command list. Lines are aliased and the sample count is not forced.
    D3D12_RASTERIZER_DESC2 rasterizerDesc(const RasterState& raster);

    /// Returns the depth-stencil state of `state`, both faces with the same masks and no depth bounds test.
    D3D12_DEPTH_STENCIL_DESC2 depthStencilDesc(const DepthStencilState& state);

    /// Returns the render target formats of `colors`, in order.
    D3D12_RT_FORMAT_ARRAY renderTargetFormats(std::span<const ColorTarget> colors);

    /// The bytecode of each stage of a pipeline; an absent stage is empty.
    struct PipelineShaders
    {
        D3D12_SHADER_BYTECODE vertex = {};   ///< The vertex entry's DXIL.
        D3D12_SHADER_BYTECODE task = {};     ///< The task entry's DXIL, streamed as the amplification shader.
        D3D12_SHADER_BYTECODE mesh = {};     ///< The mesh entry's DXIL.
        D3D12_SHADER_BYTECODE fragment = {}; ///< The fragment entry's DXIL, streamed as the pixel shader.
        D3D12_SHADER_BYTECODE compute = {};  ///< The compute entry's DXIL.
    };

    /// The bytes of one pipeline state stream, laid out as the runtime reads them: each subobject starts at a multiple of the
    /// pointer size with its `D3D12_PIPELINE_STATE_SUBOBJECT_TYPE`, and its value follows at the value's own alignment. It
    /// holds pointers (the root signature, each stage's bytecode) that must outlive the creation that reads it.
    class PipelineStream
    {
      public:
        /// Appends a subobject of `type` holding `value`. More subobjects than a graphics pipeline streams is an internal
        /// error.
        template<class Value> void add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const Value& value)
        {
            std::memcpy(place(type, alignof(Value), sizeof(Value)), &value, sizeof(Value));
        }

        /// Returns a copy of the value of the subobject of `type`, or nothing when the stream has none.
        template<class Value> std::optional<Value> find(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) const
        {
            const std::byte* bytes = locate(type);
            if (bytes == nullptr)
            {
                return std::nullopt;
            }
            Value value{};
            std::memcpy(&value, bytes, sizeof(Value));
            return value;
        }

        /// Returns true when the stream has a subobject of `type`.
        bool has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) const;

        /// Returns the description `CreatePipelineState` reads; it points into this stream.
        D3D12_PIPELINE_STATE_STREAM_DESC desc() const;

      private:
        /// Where one subobject's value lies in the stream.
        struct Placed
        {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE; ///< Its type.
            size_t offset = 0;                                                                             ///< The value's offset.
        };

        static constexpr size_t kWords = 128;        ///< The stream's capacity in 8-byte words.
        static constexpr size_t kMaxSubobjects = 16; ///< The most subobjects one stream holds.

        /// Reserves a subobject of `type` whose value has `alignment` and `size`, writes its type and returns where the value
        /// goes.
        std::byte* place(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, size_t alignment, size_t size);

        /// Returns the value of the subobject of `type`, or null when the stream has none.
        const std::byte* locate(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) const;

        std::array<uint64_t, kWords> m_words = {};        ///< The stream, zero where no subobject writes.
        size_t m_size = 0;                                ///< Bytes used, a multiple of 8.
        std::array<Placed, kMaxSubobjects> m_placed = {}; ///< The subobjects, in stream order.
        size_t m_count = 0;                               ///< Entries of `m_placed` used.
    };

    /// Returns the stream of the graphics pipeline `desc` over `root` and `shaders`: the root signature; VS and an empty
    /// input layout, or AS (with a task entry) and MS; PS with a fragment entry; then the blend, sample mask, rasterizer,
    /// depth-stencil, topology type, render target formats, depth-stencil format, sample description and flags, the flag
    /// `DYNAMIC_DEPTH_BIAS` with `raster.depthBias`. A vertex pipeline's topology type is its class's; a mesh pipeline's is
    /// the output topology its mesh entry's DXIL declares, and none when the container declares none this backend reads.
    PipelineStream graphicsStream(ID3D12RootSignature* root, const GraphicsPipelineDesc& desc, const PipelineShaders& shaders);

    /// Returns the stream of a compute pipeline over `root` and the compute entry's `bytecode`: the root signature and CS.
    PipelineStream computeStream(ID3D12RootSignature* root, D3D12_SHADER_BYTECODE bytecode);

    /// Releases the pipeline state and deletes the payload of a retiring pipeline record. `context` is unused; the hook
    /// signature carries it. A null payload is skipped.
    void retirePipeline(void* context, core::PipelineRecord& record);
} // namespace rojoRHI::d3d12
