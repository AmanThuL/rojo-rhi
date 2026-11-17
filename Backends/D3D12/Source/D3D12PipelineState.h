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
    enum class PipelineKind : uint8_t
    {
        Graphics,
        Mesh,
        Compute,
    };

    // Everything recording reads to bind the pipeline, so a bind reads this payload through `HandlePool::native` and
    // never the record. Fixed while the pipeline lives.
    struct PipelineState
    {
        ID3D12PipelineState* pso = nullptr; // One reference owned.
        PipelineKind kind = PipelineKind::Graphics;
        D3D_PRIMITIVE_TOPOLOGY listTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED; // Undefined for a mesh or compute pipeline.
        bool depthBias = false;                                                 // The list's recorded bias applies.
        bool readsStencilReference = false;
        bool readsBlendFactor = false;
    };

    D3D12_BLEND colorBlend(BlendFactor factor);

    // An alpha slot takes no colour factor: a colour factor becomes its alpha form, and a constant reads the blend
    // factor's alpha.
    D3D12_BLEND alphaBlend(BlendFactor factor);

    D3D12_BLEND_OP blendOp(BlendOp op);

    UINT8 writeMask(ColorWriteMask mask);

    D3D12_CULL_MODE cullMode(CullMode cull);

    D3D12_FILL_MODE fillMode(FillMode fill);

    D3D12_COMPARISON_FUNC comparisonFunc(CompareOp compare);

    D3D12_STENCIL_OP stencilOp(StencilOp op);

    D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType(PrimitiveClass primitive);

    D3D_PRIMITIVE_TOPOLOGY listTopology(PrimitiveClass primitive);

    // Independent blending on, alpha to coverage off; a render target past the last colour keeps the default state.
    D3D12_BLEND_DESC blendDesc(std::span<const ColorTarget> colors);

    // Its bias fields are zero: a pipeline with `depthBias` takes the bias from the command list. Lines are aliased and
    // the sample count is not forced.
    D3D12_RASTERIZER_DESC2 rasterizerDesc(const RasterState& raster);

    // Both faces share the masks; no depth bounds test.
    D3D12_DEPTH_STENCIL_DESC2 depthStencilDesc(const DepthStencilState& state);

    D3D12_RT_FORMAT_ARRAY renderTargetFormats(std::span<const ColorTarget> colors);

    struct PipelineShaders
    {
        D3D12_SHADER_BYTECODE vertex = {};
        D3D12_SHADER_BYTECODE task = {}; // Streamed as the amplification shader.
        D3D12_SHADER_BYTECODE mesh = {};
        D3D12_SHADER_BYTECODE fragment = {}; // Streamed as the pixel shader.
        D3D12_SHADER_BYTECODE compute = {};
    };

    // Laid out as the runtime reads it: each subobject starts at a multiple of the pointer size with its
    // `D3D12_PIPELINE_STATE_SUBOBJECT_TYPE`, and its value follows at the value's own alignment. The pointers it holds
    // (the root signature, each stage's bytecode) must outlive the creation that reads it.
    class PipelineStream
    {
      public:
        // More subobjects than a graphics pipeline streams is an internal error.
        template<class Value> void add(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, const Value& value)
        {
            std::memcpy(place(type, alignof(Value), sizeof(Value)), &value, sizeof(Value));
        }

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

        bool has(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) const;

        // Points into this stream.
        D3D12_PIPELINE_STATE_STREAM_DESC desc() const;

      private:
        struct Placed
        {
            D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type = D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE;
            size_t offset = 0;
        };

        static constexpr size_t kWords = 128; // In 8-byte words.
        static constexpr size_t kMaxSubobjects = 16;

        std::byte* place(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type, size_t alignment, size_t size);

        const std::byte* locate(D3D12_PIPELINE_STATE_SUBOBJECT_TYPE type) const;

        std::array<uint64_t, kWords> m_words = {}; // Zero where no subobject writes.
        size_t m_size = 0;                         // In bytes, a multiple of 8.
        std::array<Placed, kMaxSubobjects> m_placed = {};
        size_t m_count = 0;
    };

    // The root signature; VS and an empty input layout, or AS (with a task entry) and MS; PS with a fragment entry;
    // then the blend, sample mask, rasterizer, depth-stencil, topology type, render target formats, depth-stencil
    // format, sample description and flags, DYNAMIC_DEPTH_BIAS with `raster.depthBias`. A mesh pipeline's topology type
    // is the output topology its mesh entry's DXIL declares, and none when this backend reads none there.
    PipelineStream graphicsStream(ID3D12RootSignature* root, const GraphicsPipelineDesc& desc, const PipelineShaders& shaders);

    PipelineStream computeStream(ID3D12RootSignature* root, D3D12_SHADER_BYTECODE bytecode);

    // `context` is unused; a null payload is skipped.
    void retirePipeline(void* context, core::PipelineRecord& record);
} // namespace rojoRHI::d3d12
