//----------------------------------------------------------------------------------------------------------------------
/// @file NativeG1.cpp
/// @brief Implements G1's native arm on D3D12: one dispatch of 2^20 threads, each summing 16 uint4 loads from a root SRV.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/KernelArms.h"
#include "Common/KernelPatterns.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "NativeD3D12/NativeContext.h"
#include "NativeD3D12/NativeRootSignature.h"

#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::bench {
    namespace {
        constexpr uint64_t kOutputBytes = uint64_t{ kKernelThreads } * sizeof(DataEntry);

        // G1's root parameters, the layout BenchLoadsNative.hlsl reads: the data's root SRV at t0, the output's root UAV at
        // u0 and the divergent flag, one root constant at b0.
        constexpr UINT kDataParameter = 0;
        constexpr UINT kOutputParameter = 1;
        constexpr UINT kDivergentParameter = 2;

        //==============================================================================================================
        std::string failedCall(std::string_view what, HRESULT result)
        {
            return std::format("G1: {} failed with HRESULT 0x{:08X}", what, static_cast<uint32_t>(result));
        }

        class NativeG1
        {
          public:
            using Context = NativeContext;
            static constexpr Arm kArm = Arm::Native;

            NativeG1(NativeContext&, const Config&);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();

            NativeContext& m_context;
            uint32_t m_divergent = 0;
            NativeBuffer m_data{};
            NativeBuffer m_output{};
            ID3D12RootSignature* m_root = nullptr;
            ID3D12PipelineState* m_pipeline = nullptr;
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        NativeG1::NativeG1(NativeContext& context, const Config& config)
            : m_context(context), m_divergent(config.pattern.value_or("") == "divergent" ? 1u : 0u), m_ready(create())
        {
        }

        //==============================================================================================================
        std::expected<void, std::string> NativeG1::create()
        {
            const std::vector<DataEntry> table = dataTable();
            const std::expected<NativeBuffer, std::string> data =
                m_context.defaultBuffer(table.size() * sizeof(DataEntry), "bench.g1.data", std::as_bytes(std::span(table)));
            if (!data)
            {
                return std::unexpected(data.error());
            }
            m_data = *data;
            const std::expected<NativeBuffer, std::string> output = m_context.defaultBuffer(kOutputBytes, "bench.g1.output");
            if (!output)
            {
                return std::unexpected(output.error());
            }
            m_output = *output;

            D3D12_ROOT_PARAMETER1 parameters[3]{};
            parameters[kDataParameter].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
            parameters[kDataParameter].Descriptor.ShaderRegister = 0;
            parameters[kDataParameter].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
            parameters[kDataParameter].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            parameters[kOutputParameter].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            parameters[kOutputParameter].Descriptor.ShaderRegister = 0;
            parameters[kOutputParameter].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
            parameters[kOutputParameter].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            parameters[kDivergentParameter].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            parameters[kDivergentParameter].Constants.ShaderRegister = 0;
            parameters[kDivergentParameter].Constants.Num32BitValues = 1;
            parameters[kDivergentParameter].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            D3D12_ROOT_SIGNATURE_DESC1 rootDesc{};
            rootDesc.NumParameters = 3;
            rootDesc.pParameters = parameters;
            rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
            const std::expected<ID3D12RootSignature*, std::string> root = createRootSignature(m_context, rootDesc, "bench.g1.root");
            if (!root)
            {
                return std::unexpected(root.error());
            }
            m_root = *root;

            const std::expected<D3D12_SHADER_BYTECODE, std::string> shader = m_context.library("BenchLoadsNative");
            if (!shader)
            {
                return std::unexpected(shader.error());
            }
            const std::expected<ID3D12PipelineState*, std::string> pipeline = m_context.computePipeline(*shader, "bench.g1", m_root);
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            m_pipeline = *pipeline;
            return m_context.prepareFrames(NativeFrameShape{ .lists = 1 });
        }

        //==============================================================================================================
        const std::expected<void, std::string>& NativeG1::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        // Per frame: the list's Reset (a host call), then the root signature, pipeline, the two root descriptors, the root
        // constant, the dispatch and Close: seven record calls.
        std::expected<FrameResult, std::string> NativeG1::frame(uint32_t index)
        {
            NativeContext::Slot& slot = m_context.beginFrame(index, 1);
            NativeCalls& calls = m_context.calls();
            ID3D12GraphicsCommandList9* list = slot.lists[0];

            const uint64_t start = hostTicks();
            const HRESULT reset = list->Reset(slot.allocators[0], nullptr);
            ++calls.host;
            if (FAILED(reset))
            {
                (void)m_context.endFrame();
                return std::unexpected(failedCall("ID3D12GraphicsCommandList::Reset", reset));
            }
            m_context.startTimestamps(list);
            list->SetComputeRootSignature(m_root);
            list->SetPipelineState(m_pipeline);
            list->SetComputeRootShaderResourceView(kDataParameter, m_data.address);
            list->SetComputeRootUnorderedAccessView(kOutputParameter, m_output.address);
            list->SetComputeRoot32BitConstant(kDivergentParameter, m_divergent, 0);
            list->Dispatch(kKernelThreads / kKernelGroup, 1, 1);
            m_context.endTimestamps(list);
            const HRESULT closed = list->Close();
            calls.record += 7;
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
        std::expected<std::vector<Output>, std::string> NativeG1::readback()
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
    ArmResult runNativeG1(const Config& config, const RunOptions& options)
    {
        return runArm<NativeG1>(config, options);
    }
} // namespace rojoRHI::bench
