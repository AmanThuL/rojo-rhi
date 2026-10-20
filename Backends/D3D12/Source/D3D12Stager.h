//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Stager.h
/// @brief Declares the D3D12 stager, which copies a resource's initial data on the device's internal upload queue.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace rojoRHI {
    struct DeviceImpl;
}

namespace rojoRHI::d3d12 {
    /// The initial data of one subresource, in D3D12 subresource order.
    struct SubresourceData
    {
        std::span<const std::byte> bytes; ///< The subresource's texels; empty leaves its contents undefined.
        uint64_t bytesPerRow = 0;         ///< The distance between rows of `bytes`.
        uint64_t bytesPerImage = 0;       ///< The distance between depth slices of `bytes`, for a 3D texture.
    };

    /// Copies initial data into device memory on the internal upload queue, one upload at a time.
    class Stager
    {
      public:
        /// Creates the upload queue and what an upload records into on `impl`'s device.
        Result<void> create(DeviceImpl& impl);

        /// Copies `bytes` into the start of the buffer `destination` and waits for the copy.
        Result<void> uploadBuffer(DeviceImpl& impl, ID3D12Resource* destination, std::span<const std::byte> bytes);

        /// Copies each of `subresources` into the texture `destination`, created in the copy-destination layout, and waits.
        Result<void> uploadTexture(DeviceImpl& impl, ID3D12Resource* destination, std::span<const SubresourceData> subresources);

        /// Blocks until the last upload has completed.
        void idle();
    };
} // namespace rojoRHI::d3d12
