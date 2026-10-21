//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Kernels.h
/// @brief Declares the compute pipelines of the D3D12 backend's own kernels.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"

namespace rojoRHI::d3d12 {
    /// The pipelines of the backend's own kernels, created once with the device on the kernel root signature and fixed for
    /// its life. It creates none yet.
    class Kernels
    {
      public:
        /// Creates every kernel pipeline on `device` with `kernelRoot`; makes nothing and succeeds.
        Result<void> create(ID3D12Device12& device, ID3D12RootSignature& kernelRoot);

        /// Releases every pipeline.
        void release();

        /// Returns the pipeline of the fill kernel; null while none is created.
        ID3D12PipelineState* fill() const;

      private:
        Com<ID3D12PipelineState> m_fill;
    };
} // namespace rojoRHI::d3d12
