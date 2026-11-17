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
    // One growable upload buffer per frame slot, bump-allocated by the frame that writes. Single-threaded.
    //
    // `newFrame` only arms an advance: the first `allocate` after it moves to the next slot and rewinds it, so the ring
    // advances once per frame that wrote. A slot is reused three writing frames later, and every writing frame records into
    // a device frame, so the slot's earlier frame has retired by then on a device with at most three frames in flight. A
    // request that does not fit retires the slot's buffer and moves to a larger one at offset 0; retired buffers are
    // destroyed at the next `newFrame`, after every list of the frame that used them was submitted.
    class FrameRing
    {
      public:
        // The largest `DeviceDesc::framesInFlight`.
        static constexpr uint32_t kFrameSlots = 3;

        // Valid for the current frame.
        struct Allocation
        {
            Buffer buffer;
            uint64_t offset;
            std::byte* cpu;
            Addr address;
        };

        FrameRing(Device device, std::string_view label);

        // `alignment` is a power of two. Failing to create a larger buffer is fatal.
        Allocation allocate(uint64_t size, uint64_t alignment);

        // Called with no list of the previous frame left unsubmitted, because it destroys the buffers growth retired.
        void newFrame();

        void destroy();

      private:
        struct Slot
        {
            Buffer buffer;
            Addr base;
            std::byte* cpu = nullptr;
            uint64_t capacity = 0;
            uint64_t used = 0;
        };

        void grow(uint64_t request);

        Device m_device;
        std::string m_label;
        Slot m_slots[kFrameSlots];
        uint32_t m_current = kFrameSlots - 1;
        bool m_advance = true;
        std::vector<Buffer> m_retired;
    };
} // namespace rojoRHI::imgui::detail
