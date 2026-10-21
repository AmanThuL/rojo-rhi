//----------------------------------------------------------------------------------------------------------------------
/// @file NativeG2.cpp
/// @brief Implements G2's native arms on D3D12: arm T samples 128 textures through heap indices, arm F through a descriptor table.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/KernelArms.h"
#include "Common/KernelPatterns.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "NativeD3D12/NativeContext.h"
#include "NativeD3D12/NativeRootSignature.h"

#include <array>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::bench {
    namespace {
        constexpr uint64_t kOutputBytes = uint64_t{ kKernelThreads } * sizeof(float);

        // The root parameters both arms share, the layouts BenchSamplesTable.hlsl and BenchSamplesFixed.hlsl read: the
        // output's root UAV at u0 and the root constants at b0. Arm T's constants are the first view's heap index, the
        // sampler's heap index and the divergent flag; arm F's are the divergent flag alone, and its descriptor table of the
        // 128 SRVs follows them.
        constexpr UINT kOutputParameter = 0;
        constexpr UINT kConstantsParameter = 1;
        constexpr UINT kTableParameter = 2;

        // How the kernel reaches texture i: through ResourceDescriptorHeap and SamplerDescriptorHeap indices from the root
        // constants, or from a descriptor table bound in the root signature, with a static sampler.
        enum class TextureAccess : uint8_t
        {
            HeapIndices,
            DescriptorTable,
        };

        //==============================================================================================================
        std::string failedCall(std::string_view what, HRESULT result)
        {
            return std::format("G2: {} failed with HRESULT 0x{:08X}", what, static_cast<uint32_t>(result));
        }

        // The setup and frame both arms share; only the root signature, the kernel, the binding calls and the arm name differ.
        class NativeSamples
        {
          public:
            NativeSamples(NativeContext&, const Config&, TextureAccess);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();
            std::expected<void, std::string> createTextures();
            std::expected<void, std::string> createRoot();

            NativeContext& m_context;
            TextureAccess m_access;
            uint32_t m_divergent = 0;
            uint32_t m_viewBase = 0;
            uint32_t m_samplerIndex = 0;
            NativeBuffer m_output{};
            ID3D12RootSignature* m_root = nullptr;
            ID3D12PipelineState* m_pipeline = nullptr;
            std::expected<void, std::string> m_ready;
        };

        class NativeG2 : public NativeSamples
        {
          public:
            using Context = NativeContext;
            static constexpr Arm kArm = Arm::Native;

            NativeG2(NativeContext&, const Config&);
        };

        class FixedG2 : public NativeSamples
        {
          public:
            using Context = NativeContext;
            static constexpr Arm kArm = Arm::Fixed;

            FixedG2(NativeContext&, const Config&);
        };

        //==============================================================================================================
        NativeSamples::NativeSamples(NativeContext& context, const Config& config, TextureAccess access)
            : m_context(context), m_access(access), m_divergent(config.pattern.value_or("") == "divergent" ? 1u : 0u), m_ready(create())
        {
        }

        //==============================================================================================================
        NativeG2::NativeG2(NativeContext& context, const Config& config) : NativeSamples(context, config, TextureAccess::HeapIndices)
        {
        }

        //==============================================================================================================
        FixedG2::FixedG2(NativeContext& context, const Config& config) : NativeSamples(context, config, TextureAccess::DescriptorTable)
        {
        }

        //==============================================================================================================
        // The 128 R32Float textures of 16 by 16 texels, each written from tight rows in one waited setup submission and left
        // in LAYOUT_SHADER_RESOURCE, and their SRVs in consecutive view-heap slots from m_viewBase, which a descriptor table
        // and the heap indices both name.
        std::expected<void, std::string> NativeSamples::createTextures()
        {
            D3D12_RESOURCE_DESC1 desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = kTextureSize;
            desc.Height = kTextureSize;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R32_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags = D3D12_RESOURCE_FLAG_NONE;
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R32_FLOAT;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MostDetailedMip = 0;
            view.Texture2D.MipLevels = 1;
            view.Texture2D.PlaneSlice = 0;
            view.Texture2D.ResourceMinLODClamp = 0.0f;
            for (uint32_t t = 0; t < kTextureCount; ++t)
            {
                const std::expected<ID3D12Resource*, std::string> texture =
                    m_context.texture(desc, D3D12_BARRIER_LAYOUT_COPY_DEST, std::format("bench.g2.texture.{}", t));
                if (!texture)
                {
                    return std::unexpected(texture.error());
                }
                const std::vector<float> texels = textureTexels(t);
                const std::expected<void, std::string> written =
                    m_context.writeTexture(*texture, 0, std::as_bytes(std::span(texels)), D3D12_BARRIER_LAYOUT_COPY_DEST, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
                if (!written)
                {
                    return std::unexpected(std::format("G2: texture {}: {}", t, written.error()));
                }
                const std::expected<uint32_t, std::string> index = m_context.shaderResourceView(*texture, view);
                if (!index)
                {
                    return std::unexpected(index.error());
                }
                if (t == 0)
                {
                    m_viewBase = *index;
                }
                else if (*index != m_viewBase + t)
                {
                    return std::unexpected(std::format("G2: the view of texture {} is at heap index {}, not {}", t, *index, m_viewBase + t));
                }
            }
            return {};
        }

        //==============================================================================================================
        // Arm T's root signature indexes both heaps directly and has three root constants; arm F's has one root constant, a
        // descriptor table of the 128 SRVs at t0 to t127 and a static point sampler at s0, clamped to the edge, which the
        // global root signature does not have.
        std::expected<void, std::string> NativeSamples::createRoot()
        {
            const bool heap = m_access == TextureAccess::HeapIndices;
            D3D12_ROOT_PARAMETER1 parameters[3]{};
            parameters[kOutputParameter].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            parameters[kOutputParameter].Descriptor.ShaderRegister = 0;
            parameters[kOutputParameter].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
            parameters[kOutputParameter].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            parameters[kConstantsParameter].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameters[kConstantsParameter].Constants.ShaderRegister = 0;
            parameters[kConstantsParameter].Constants.Num32BitValues = heap ? 3 : 1;
            parameters[kConstantsParameter].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            const D3D12_DESCRIPTOR_RANGE1 range{
                D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kTextureCount, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_NONE, 0,
            };
            parameters[kTableParameter].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[kTableParameter].DescriptorTable.NumDescriptorRanges = 1;
            parameters[kTableParameter].DescriptorTable.pDescriptorRanges = &range;
            parameters[kTableParameter].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            D3D12_STATIC_SAMPLER_DESC nearest{};
            nearest.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            nearest.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            nearest.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            nearest.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            nearest.MipLODBias = 0.0f;
            nearest.MaxAnisotropy = 1;
            nearest.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            nearest.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
            nearest.MinLOD = 0.0f;
            nearest.MaxLOD = D3D12_FLOAT32_MAX;
            nearest.ShaderRegister = 0;
            nearest.RegisterSpace = 0;
            nearest.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_ROOT_SIGNATURE_DESC1 desc{};
            desc.pParameters = parameters;
            if (heap)
            {
                desc.NumParameters = 2;
                desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED | D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
            }
            else
            {
                desc.NumParameters = 3;
                desc.NumStaticSamplers = 1;
                desc.pStaticSamplers = &nearest;
                desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
            }
            const std::expected<ID3D12RootSignature*, std::string> root =
                createRootSignature(m_context, desc, heap ? "bench.g2.root.heap" : "bench.g2.root.fixed");
            if (!root)
            {
                return std::unexpected(root.error());
            }
            m_root = *root;
            return {};
        }

        //==============================================================================================================
        std::expected<void, std::string> NativeSamples::create()
        {
            if (const std::expected<void, std::string> textures = createTextures(); !textures)
            {
                return textures;
            }
            const std::expected<NativeBuffer, std::string> output = m_context.defaultBuffer(kOutputBytes, "bench.g2.output");
            if (!output)
            {
                return std::unexpected(output.error());
            }
            m_output = *output;
            const bool heap = m_access == TextureAccess::HeapIndices;
            if (heap)
            {
                // The sampler R creates: point filtering for minification, magnification and mips, clamped to the edge.
                D3D12_SAMPLER_DESC sampler{};
                sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                sampler.MipLODBias = 0.0f;
                sampler.MaxAnisotropy = 1;
                sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
                sampler.MinLOD = 0.0f;
                sampler.MaxLOD = D3D12_FLOAT32_MAX;
                const std::expected<uint32_t, std::string> index = m_context.sampler(sampler);
                if (!index)
                {
                    return std::unexpected(index.error());
                }
                m_samplerIndex = *index;
            }
            if (const std::expected<void, std::string> rooted = createRoot(); !rooted)
            {
                return rooted;
            }
            const std::expected<D3D12_SHADER_BYTECODE, std::string> shader = m_context.library(heap ? "BenchSamplesTable" : "BenchSamplesFixed");
            if (!shader)
            {
                return std::unexpected(shader.error());
            }
            const std::expected<ID3D12PipelineState*, std::string> pipeline =
                m_context.computePipeline(*shader, heap ? "bench.g2.heap" : "bench.g2.fixed", m_root);
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            m_pipeline = *pipeline;
            return m_context.prepareFrames(NativeFrameShape{ .lists = 1 });
        }

        //==============================================================================================================
        const std::expected<void, std::string>& NativeSamples::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        // Per frame: the list's Reset (a host call), then the descriptor heaps (before the root signature, which indexes
        // them or holds a table in them), root signature, pipeline, root UAV and root constants, arm F's descriptor table,
        // the dispatch and Close: seven record calls on arm T, eight on arm F.
        std::expected<FrameResult, std::string> NativeSamples::frame(uint32_t index)
        {
            NativeContext::Slot& slot = m_context.beginFrame(index, 1);
            NativeCalls& calls = m_context.calls();
            ID3D12GraphicsCommandList9* list = slot.lists[0];
            const std::array<uint32_t, 3> constants{ m_viewBase, m_samplerIndex, m_divergent };
            const bool heap = m_access == TextureAccess::HeapIndices;
            const std::span<ID3D12DescriptorHeap* const> heaps = heap ? m_context.heaps() : m_context.heaps().first(1);

            const uint64_t start = hostTicks();
            const HRESULT reset = list->Reset(slot.allocators[0], nullptr);
            ++calls.host;
            if (FAILED(reset))
            {
                (void)m_context.endFrame();
                return std::unexpected(failedCall("ID3D12GraphicsCommandList::Reset", reset));
            }
            list->SetDescriptorHeaps(static_cast<UINT>(heaps.size()), heaps.data());
            list->SetComputeRootSignature(m_root);
            list->SetPipelineState(m_pipeline);
            list->SetComputeRootUnorderedAccessView(kOutputParameter, m_output.address);
            calls.record += 4;
            if (heap)
            {
                list->SetComputeRoot32BitConstants(kConstantsParameter, static_cast<UINT>(constants.size()), constants.data(), 0);
                ++calls.record;
            }
            else
            {
                list->SetComputeRoot32BitConstant(kConstantsParameter, m_divergent, 0);
                list->SetComputeRootDescriptorTable(kTableParameter, m_context.viewGpu(m_viewBase));
                calls.record += 2;
            }
            list->Dispatch(kKernelThreads / kKernelGroup, 1, 1);
            const HRESULT closed = list->Close();
            calls.record += 2;
            const uint64_t end = hostTicks();
            if (FAILED(closed))
            {
                (void)m_context.endFrame();
                return std::unexpected(failedCall("ID3D12GraphicsCommandList::Close", closed));
            }

            ID3D12CommandList* const lists[] = { list };
            m_context.commit(m_context.graphicsQueue(), lists);
            const std::expected<std::optional<double>, std::string> gpu = m_context.endFrame();
            if (!gpu)
            {
                return std::unexpected(gpu.error());
            }
            return FrameResult{ .cpuNs = ticksToNanoseconds(end - start), .gpuNs = *gpu };
        }

        //==============================================================================================================
        std::expected<std::vector<Output>, std::string> NativeSamples::readback()
        {
            std::expected<std::vector<std::byte>, std::string> bytes = m_context.readBuffer(m_output.resource, 0, kOutputBytes);
            if (!bytes)
            {
                return std::unexpected(bytes.error());
            }
            std::vector<Output> outputs;
            outputs.push_back(Output{ "output", std::move(*bytes) });
            return outputs;
        }
    } // namespace

    //==================================================================================================================
    ArmResult runNativeG2(const Config& config, const RunOptions& options)
    {
        return runArm<NativeG2>(config, options);
    }

    //==================================================================================================================
    ArmResult runFixedG2(const Config& config, const RunOptions& options)
    {
        return runArm<FixedG2>(config, options);
    }
} // namespace rojoRHI::bench
