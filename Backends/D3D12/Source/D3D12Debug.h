//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Debug.h
/// @brief Declares the D3D12 debug-layer message routing and the report and error of a removed device.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"

#include <string_view>

namespace rojoRHI {
    struct DeviceImpl;
}

namespace rojoRHI::d3d12 {
    /// Routes the debug layer's messages of one device into the library's message callback while it is attached.
    class MessageRouter
    {
      public:
        /// Starts routing the messages of `device` when `layer`, the build's debug layer, is on; does nothing otherwise.
        void attach(ID3D12Device12* device, bool layer);

        /// Stops routing; does nothing when not attached.
        void detach();
    };

    /// Returns true when `result` reports a removed, reset or hung device.
    [[nodiscard]] bool deviceRemoved(HRESULT result);

    /// Reports why `impl`'s device was removed, as `Error` messages naming `entry`, the entry that found it removed.
    void reportRemoval(DeviceImpl& impl, std::string_view entry);

    /// Returns the error of a native call `what` that failed with `result`, after reporting the removal when the device was
    /// removed.
    Error failure(DeviceImpl& impl, HRESULT result, std::string_view what);
} // namespace rojoRHI::d3d12
