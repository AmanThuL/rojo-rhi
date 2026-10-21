//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Kernels.h
/// @brief Declares the compute pipelines of the D3D12 backend's own kernels and what the fill kernel reads.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"

#include <cstdint>

namespace rojoRHI::d3d12 {
    /// Threads per group of the fill kernel, its `numthreads`.
    inline constexpr uint32_t kFillGroupThreads = 64;

    /// The root constants of one fill dispatch, laid out as `Kernels/Fill.hlsl` reads them at `b0`.
    struct FillConstants
    {
        uint32_t uavIndex = 0;  ///< The view-heap index of the raw UAV the dispatch writes through.
        uint32_t pattern = 0;   ///< The word written, its bytes in little-endian order.
        uint32_t firstWord = 0; ///< The first word written, counted from the view's first element.
        uint32_t wordCount = 0; ///< How many words the dispatch writes, at most the 2^27 one raw view reaches.
    };
    static_assert(sizeof(FillConstants) == 16, "FillConstants must be the four root constants of the kernel root signature");

    /// Returns the groups one fill dispatch of `words` words runs: one per `kFillGroupThreads` words or part of it, at most
    /// `D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION`. The kernel computes the same count, and its threads loop over the
    /// words past the grid with a stride of the grid's threads.
    uint32_t fillGroups(uint64_t words);

    /// The pipelines of the backend's own kernels, created once with the device on the kernel root signature and fixed for
    /// its life.
    class Kernels
    {
      public:
        /// Creates the fill kernel's compute pipeline on `device` with `kernelRoot` from the DXIL the build embeds, named
        /// `rojorhi.kernel.fill`. A refused creation is the error `hresultError` gives, naming the fill kernel.
        Result<void> create(ID3D12Device12& device, ID3D12RootSignature& kernelRoot);

        /// Releases every pipeline.
        void release();

        /// Returns the pipeline of the fill kernel; null before `create` succeeds.
        ID3D12PipelineState* fill() const;

      private:
        Com<ID3D12PipelineState> m_fill;
    };
} // namespace rojoRHI::d3d12
