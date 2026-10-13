//----------------------------------------------------------------------------------------------------------------------
/// @file CommandList.h
/// @brief Declares the command list object handle.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/Addr.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace rojoRHI {
    /// A recorded sequence of commands for one queue type. It wraps one backend pointer: copies refer to the same list, and a
    /// default-constructed one refers to none (VU-rojo-core-0004). It comes from a per-frame pool and returns to it when its
    /// frame retires; using it after that is undefined behaviour no tier detects. It is used only with the device that
    /// acquired it (VU-rojo-core-0005). One thread records one list at a time and recording takes no locks, except when `uploadFrameData` takes a new page from
    /// the frame arena.
    class CommandList
    {
      public:
        /// Ends recording; the list then records nothing further and is ready for `Queue::submit`. The list is recording
        /// (VU-rojo-cmd-0002), no render pass is open (VU-rojo-cmd-0003) and every scope begun on it has ended
        /// (VU-rojo-cmd-0004). Thread class command list.
        void end();

        /// Copies `bytes` into the open frame's arena at an address aligned to `alignment` and returns that address, valid
        /// only within the frame that produced it. `bytes` is not empty (VU-rojo-buffer-0015), `alignment` is a power of two
        /// (VU-rojo-buffer-0016), a frame is open (VU-rojo-buffer-0017) and only work submitted before that frame's
        /// `Device::endFrame` reads the address (VU-rojo-buffer-0018). Thread class command list.
        Addr uploadFrameData(std::span<const std::byte> bytes, uint32_t alignment = 256);

      private:
        friend struct CommandListImpl;
        struct CommandListImpl* m_impl = nullptr;
    };
} // namespace rojoRHI
