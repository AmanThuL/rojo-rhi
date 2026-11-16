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

    /// Returns the address of byte `offset` of the buffer `payload` describes: its raw SRV's view-heap index and the offset.
    /// The caller has checked the offset against the buffer and `kMaxAddrOffset`; every address the backend hands out is
    /// encoded here.
    Addr encodeAddr(const BufferPayload& payload, uint64_t offset);

    /// Finds the buffer whose raw SRV sits at a view-heap index, so an address decodes without a lock. An entry is published
    /// when its buffer is created, before the handle is returned, and cleared by the buffer's retire hook before the payload
    /// is freed; `publish` and `clear` take the directory's own mutex, a leaf no other lock is taken under, and `find` is one
    /// acquire load.
    class HeapDirectory
    {
      public:
        /// Sizes the directory for a view heap of `capacity` descriptors, at most `core::NativeTable::kCapacity`.
        void create(uint32_t capacity);

        /// Records that the raw SRV at `index` belongs to `payload`.
        void publish(uint32_t index, const BufferPayload* payload);

        /// Forgets the buffer at `index`.
        void clear(uint32_t index);

        /// Returns the payload of the buffer whose raw SRV is at `index`, or null, also for an index past the view heap.
        const BufferPayload* find(uint32_t index) const;

      private:
        std::unique_ptr<core::NativeTable> m_table; ///< The payload of each view-heap index; null until `create`.
        std::mutex m_mutex;                         ///< Serialises `publish` and `clear`.
    };

    /// Returns the GPU virtual address `addr` names: the base of the buffer whose raw SRV sits at its heap index, found in
    /// `directory` without a lock, plus its byte offset; 0 when no buffer is there.
    D3D12_GPU_VIRTUAL_ADDRESS rootAddress(const HeapDirectory& directory, Addr addr);
} // namespace rojoRHI::d3d12
