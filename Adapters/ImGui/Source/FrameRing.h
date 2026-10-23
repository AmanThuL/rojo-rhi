//----------------------------------------------------------------------------------------------------------------------
/// @file FrameRing.h
/// @brief Declares the per-frame-slot growable upload buffer the ImGui renderer takes vertices, indices and staging from.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace rojoRHI::imgui::detail {
    /// One growable `Memory::Upload | BufferUsage::Index` buffer per frame slot, bump-allocated by the frame that writes.
    ///
    /// `newFrame` only arms an advance: the first `allocate` after it moves to the next slot and rewinds it, so the ring
    /// advances once per frame that wrote. A slot is reused three writing frames later, and every writing frame records into
    /// a device frame, so the slot's earlier frame has retired by then on a device with at most three frames in flight. A
    /// request that does not fit retires the slot's buffer and moves to a larger one at offset 0; retired buffers are
    /// destroyed at the next `newFrame`, after every list of the frame that used them was submitted. Single-threaded.
    class FrameRing
    {
      public:
        /// The slot count: the largest `DeviceDesc::framesInFlight`.
        static constexpr uint32_t kFrameSlots = 3;

        /// Bytes written by the CPU and read by the GPU in the current frame.
        struct Allocation
        {
            Buffer buffer;   ///< The slot's buffer holding the bytes.
            uint64_t offset; ///< Bytes from the buffer's start.
            std::byte* cpu;  ///< Where the CPU writes them.
            Addr address;    ///< Their device address.
        };

        /// Creates no buffer yet; `label` prefixes each buffer's label as `<label>.frame<slot>`.
        FrameRing(Device device, std::string_view label);

        /// Returns `size` bytes at a multiple of `alignment` (a power of two) in the current slot. A failure to create a
        /// larger buffer is fatal: a message on stderr, then `std::abort()`.
        Allocation allocate(uint64_t size, uint64_t alignment);

        /// Arms the advance to the next slot and destroys the buffers retired by growth. Called with no list of the previous
        /// frame left unsubmitted.
        void newFrame();

        /// Destroys every buffer, retired ones included.
        void destroy();

      private:
        /// One frame slot's buffer and its bump offset.
        struct Slot
        {
            Buffer buffer;
            Addr base;
            std::byte* cpu = nullptr;
            uint64_t capacity = 0;
            uint64_t used = 0;
        };

        /// Replaces the current slot's buffer with one of at least `request` bytes, retiring the old one.
        void grow(uint64_t request);

        Device m_device;
        std::string m_label;
        Slot m_slots[kFrameSlots];
        uint32_t m_current = kFrameSlots - 1;
        bool m_advance = true;
        std::vector<Buffer> m_retired;
    };
} // namespace rojoRHI::imgui::detail
