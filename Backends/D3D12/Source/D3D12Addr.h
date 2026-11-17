//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Addr.h
/// @brief Declares the D3D12 lowering of an Addr: its encoding, the buffer payload it names and the heap directory that finds it.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/NativeTable.h"
#include "D3D12Common.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>

namespace rojoRHI::d3d12 {
    // A raw view holds at most 2^27 four-byte elements.
    inline constexpr uint64_t kMaxAddrOffset = (1ull << 29) - 1;

    // Fixed from creation to retirement, when the buffer's retire hook frees it.
    struct BufferPayload
    {
        ID3D12Resource* resource = nullptr; // One reference owned.
        D3D12_GPU_VIRTUAL_ADDRESS base = 0;
        uint64_t size = 0; // In bytes, as the caller asked; the resource's width is rounded up to a multiple of 4.
        uint64_t bufferBits = 0;
        uint32_t viewIndex = 0;    // The raw SRV's view-heap index: the `lo` of every address of the buffer.
        uint32_t viewCount = 0;    // 0, 1 (SRV) or 2 (SRV, then the UAV at `viewIndex + 1`).
        std::byte* host = nullptr; // Upload or Readback memory only.
    };

    // The caller has checked `offset` against the buffer and `kMaxAddrOffset`.
    Addr encodeAddr(const BufferPayload& payload, uint64_t offset);

    // Maps a view-heap index to the buffer whose raw SRV sits there, so an address decodes without a lock. An entry is
    // published before the buffer's handle is returned and cleared by its retire hook before the payload is freed.
    // `publish` and `clear` take a leaf mutex no other lock is taken under; `find` is one acquire load.
    class HeapDirectory
    {
      public:
        // `capacity` is at most `core::NativeTable::kCapacity`.
        void create(uint32_t capacity);

        void publish(uint32_t index, const BufferPayload* payload);

        void clear(uint32_t index);

        const BufferPayload* find(uint32_t index) const;

      private:
        std::unique_ptr<core::NativeTable> m_table;
        std::mutex m_mutex;
    };

    // 0 when no buffer's raw SRV sits at `addr.lo`.
    D3D12_GPU_VIRTUAL_ADDRESS rootAddress(const HeapDirectory& directory, Addr addr);
} // namespace rojoRHI::d3d12
