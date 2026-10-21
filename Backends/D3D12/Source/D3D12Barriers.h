//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Barriers.h
/// @brief Declares the scratch a D3D12 command list lowers its barrier batches into.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

namespace rojoRHI::d3d12 {
    /// The arrays a command list lowers one barrier batch into before its native `Barrier` call; the list's own, so
    /// recording takes no lock, and grown, never shrunk.
    struct BarrierScratch
    {
    };
} // namespace rojoRHI::d3d12
