//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Addr.cpp
/// @brief Implements the D3D12 address lowering: the heap directory and the encoding, decoding and offsetting of an Addr.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Addr.h"

#include <rojoRHI/RHI.h>

#include "Base/Assert.h"
#include "Core/Addresses.h"
#include "Core/Buffers.h"
#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "Core/NativeTable.h"
#include "Core/Records.h"
#include "D3D12Device.h"

#include <cstdint>
#include <memory>
#include <mutex>

namespace rojoRHI {
    // The D3D12 encoding of an Addr is defined here and in the D3D12 branch of the shader module alone: `lo` is the
    // view-heap index of the buffer's raw SRV, whose raw UAV, when it has one, sits at `lo + 1`, and `hi` is the byte offset
    // into the buffer. No other file of the backend encodes, decodes or offsets an address.
    namespace d3d12 {
        //==============================================================================================================
        Addr encodeAddr(const BufferPayload& payload, uint64_t offset)
        {
            ROJORHI_INTERNAL_ASSERT(payload.viewCount > 0, "an address of a buffer with no raw view");
            ROJORHI_INTERNAL_ASSERT(offset < kMaxAddrOffset, "an address offset past maxAddrOffset");
            return Addr{ payload.viewIndex, static_cast<uint32_t>(offset) };
        }

        //==============================================================================================================
        void HeapDirectory::create(uint32_t capacity)
        {
            m_table = std::make_unique<core::NativeTable>(capacity);
        }

        //==============================================================================================================
        void HeapDirectory::publish(uint32_t index, const BufferPayload* payload)
        {
            std::lock_guard lock(m_mutex);
            // The table stores untyped pointers; `find` hands the payload back as const.
            m_table->publish(index, const_cast<BufferPayload*>(payload));
        }

        //==============================================================================================================
        void HeapDirectory::clear(uint32_t index)
        {
            std::lock_guard lock(m_mutex);
            m_table->publish(index, nullptr);
        }

        //==============================================================================================================
        const BufferPayload* HeapDirectory::find(uint32_t index) const
        {
            // The table wraps an index past its directory, so an index past the heap is refused here instead.
            return index < m_table->capacity() ? static_cast<const BufferPayload*>(m_table->load(index)) : nullptr;
        }
    } // namespace d3d12

    namespace core {
        //==============================================================================================================
        DecodedAddr decodeAddr(DeviceCore& core, Addr addr)
        {
            const DeviceImpl& impl = *static_cast<const DeviceImpl*>(core.retireHooks().context);
            const d3d12::BufferPayload* payload = impl.directory.find(addr.lo);
            if (payload == nullptr)
            {
                // Slot 0 names no buffer, so resolveAddr reports an address outside every live buffer.
                return DecodedAddr{ 0, 0 };
            }
            Buffer buffer;
            buffer.bits = payload->bufferBits;
            return DecodedAddr{ HandlePool<Buffer, BufferRecord>::slotIndex(buffer), addr.hi };
        }
    } // namespace core

    //==================================================================================================================
    Addr operator+(Addr base, uint64_t offset)
    {
        // The byte offset lives in `hi`; the offset is checked alone first so the sum cannot wrap.
        core::checkAddrOffset(offset, d3d12::kMaxAddrOffset);
        const uint64_t sum = static_cast<uint64_t>(base.hi) + offset;
        core::checkAddrOffset(sum, d3d12::kMaxAddrOffset);
        return Addr{ base.lo, static_cast<uint32_t>(sum) };
    }
} // namespace rojoRHI
