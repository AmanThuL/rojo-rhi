//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Addr.h
/// @brief Declares the D3D12 lowering of an Addr: the buffer payload its raw views name and the heap directory that finds it.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"

#include <cstddef>
#include <cstdint>

namespace rojoRHI::d3d12 {
    /// The greatest byte offset one `Addr` reaches: a raw view holds at most 2^27 four-byte elements.
    inline constexpr uint64_t kMaxAddrOffset = (1ull << 29) - 1;

    /// The payload of a buffer record: the resource, one reference owned, and what an address of it needs, fixed from
    /// creation to retirement, when the retire hook frees it.
    struct BufferPayload
    {
        ID3D12Resource* resource = nullptr; ///< The native buffer, one reference owned.
        D3D12_GPU_VIRTUAL_ADDRESS base = 0; ///< The buffer's GPU virtual address.
        uint64_t size = 0;                  ///< The size the caller asked for, in bytes.
        uint64_t bufferBits = 0;            ///< The bits of the buffer's handle.
        uint32_t viewIndex = 0;             ///< The view-heap index of the raw SRV, the `lo` of every address of the buffer.
        uint32_t viewCount = 0;             ///< The raw views the buffer holds: 0, 1 (SRV) or 2 (SRV and UAV).
        std::byte* host = nullptr;          ///< The mapped memory of an Upload or Readback buffer; null otherwise.
    };

    /// Finds the buffer whose raw SRV sits at a view-heap index, so an address decodes without a lock.
    class HeapDirectory
    {
      public:
        /// Sizes the directory for a view heap of `capacity` descriptors.
        void create(uint32_t capacity);

        /// Records that the raw SRV at `index` belongs to `payload`.
        void publish(uint32_t index, const BufferPayload* payload);

        /// Forgets the buffer at `index`.
        void clear(uint32_t index);

        /// Returns the payload of the buffer whose raw SRV is at `index`, or null.
        const BufferPayload* find(uint32_t index) const;
    };
} // namespace rojoRHI::d3d12
