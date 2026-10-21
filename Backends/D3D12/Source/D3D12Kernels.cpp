//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Kernels.cpp
/// @brief Implements the D3D12 backend's kernel pipelines, of which none is created yet.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Kernels.h"

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"

namespace rojoRHI::d3d12 {
    //==================================================================================================================
    Result<void> Kernels::create(ID3D12Device12&, ID3D12RootSignature&)
    {
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
