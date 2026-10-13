//----------------------------------------------------------------------------------------------------------------------
/// @file PresentMode.h
/// @brief Declares when a presented swapchain image reaches the display.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>

namespace rojoRHI {
    /// When a presented image reaches the display. A mode the surface or display cannot provide makes
    /// `Device::createSwapchain` return an error; `Fifo` is always available.
    enum class PresentMode : uint8_t
    {
        Fifo,      ///< Each image is shown for at least one vertical blank, in present order; never tears.
        Mailbox,   ///< Shown at the next vertical blank; a newer present replaces a queued one; does not tear.
        Immediate, ///< Shown as soon as possible; may tear.
    };
} // namespace rojoRHI
