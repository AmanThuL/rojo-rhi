//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Kernels.h
/// @brief Declares the compute pipelines of the D3D12 backend's own kernels and what the fill kernel reads.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"

#include <cstdint>

namespace rojoRHI::d3d12 {
    // The fill kernel's `numthreads`.
    inline constexpr uint32_t kFillGroupThreads = 64;

    // 2 GiB: the kernel addresses its words by 32-bit byte offsets from the root UAV at the window's start.
    inline constexpr uint64_t kFillWindowBytes = uint64_t{ 1 } << 31;

    // Laid out as `Kernels/Fill.hlsl` reads them at b0.
    struct FillConstants
    {
        uint32_t pattern = 0;   // Its bytes in little-endian order.
        uint32_t wordCount = 0; // At most 2^29.
    };
    static_assert(sizeof(FillConstants) == 8, "FillConstants must be the two root constants of the kernel root signature");

    // At most D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION. The kernel computes the same count, and its threads
    // stride over the words past the grid.
    uint32_t fillGroups(uint64_t words);

    // Created once with the device on the kernel root signature and fixed for its life.
    class Kernels
    {
      public:
        Result<void> create(ID3D12Device12& device, ID3D12RootSignature& kernelRoot);

        void release();

        ID3D12PipelineState* fill() const;

      private:
        Com<ID3D12PipelineState> m_fill;
    };
} // namespace rojoRHI::d3d12
