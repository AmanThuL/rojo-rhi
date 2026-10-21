//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Kernels.cpp
/// @brief Implements the D3D12 backend's kernel pipelines, built from the DXIL the build embeds.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Kernels.h"

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"
#include "D3D12KernelLibrary.h"

#include <algorithm>
#include <cstdint>
#include <expected>

namespace rojoRHI::d3d12 {
    //==================================================================================================================
    uint32_t fillGroups(uint64_t words)
    {
        const uint64_t groups = (words + kFillGroupThreads - 1) / kFillGroupThreads;
        return static_cast<uint32_t>(std::min<uint64_t>(groups, D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION));
    }

    //==================================================================================================================
    Result<void> Kernels::create(ID3D12Device12& device, ID3D12RootSignature& kernelRoot)
    {
        // The shader carries no root signature: the pipeline names the kernel root signature.
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = &kernelRoot;
        desc.CS.pShaderBytecode = kFillKernel;
        desc.CS.BytecodeLength = sizeof(kFillKernel);
        desc.NodeMask = 0;
        desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
        const HRESULT result = device.CreateComputePipelineState(&desc, IID_PPV_ARGS(m_fill.put()));
        if (FAILED(result))
        {
            return std::unexpected(hresultError(result, "ID3D12Device::CreateComputePipelineState for the fill kernel"));
        }
        setName(m_fill.get(), "rojorhi.kernel.fill");
        return {};
    }

    //==================================================================================================================
    void Kernels::release()
    {
        m_fill.reset();
    }

    //==================================================================================================================
    ID3D12PipelineState* Kernels::fill() const
    {
        return m_fill.get();
    }
} // namespace rojoRHI::d3d12
