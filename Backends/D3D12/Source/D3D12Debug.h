//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Debug.h
/// @brief Declares the D3D12 debug-layer message routing and the report and error of a removed device.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"

#include <d3d12sdklayers.h>

#include <string_view>

namespace rojoRHI {
    struct DeviceImpl;
}

namespace rojoRHI::d3d12 {
    /// Routes the debug layer's messages of one device into the library's message callback while it is attached. A message of
    /// severity error or corruption aborts the process once it has been forwarded. The runtime may raise a message on any
    /// thread, so the installed message callback may run on any thread.
    class MessageRouter
    {
      public:
        MessageRouter() = default;
        MessageRouter(const MessageRouter&) = delete;
        MessageRouter& operator=(const MessageRouter&) = delete;

        /// Detaches.
        ~MessageRouter();

        /// Starts routing the messages of `device` when `layer`, the build's debug layer, is on; does nothing otherwise. The
        /// router keeps its own reference to the device's message queue, so it detaches without the device.
        void attach(ID3D12Device12* device, bool layer);

        /// Stops routing; does nothing when not attached.
        void detach();

      private:
        Com<ID3D12InfoQueue1> m_queue; ///< The queue the callback is registered on; null when not attached.
        DWORD m_cookie = 0;            ///< What `RegisterMessageCallback` returned, which `detach` unregisters.
    };

    /// Returns true when `result` reports a removed, reset or hung device.
    [[nodiscard]] bool deviceRemoved(HRESULT result);

    /// Reports why `impl`'s device was removed, as `Error` messages naming `entry`, the entry that found it removed: the
    /// reason, then the auto-breadcrumb nodes and the page fault when the device-removed extended data is on.
    void reportRemoval(DeviceImpl& impl, std::string_view entry);

    /// Returns the error of a native call `what` that failed with `result`, after reporting the removal when the device was
    /// removed.
    Error failure(DeviceImpl& impl, HRESULT result, std::string_view what);
} // namespace rojoRHI::d3d12
