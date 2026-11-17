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
    // The runtime may raise a message on any thread, so the library's message callback may run on any thread. A message
    // of severity error or corruption aborts the process once it has been forwarded, except the runtime's
    // device-removal messages, which the backend's own removal report follows.
    class MessageRouter
    {
      public:
        MessageRouter() = default;
        MessageRouter(const MessageRouter&) = delete;
        MessageRouter& operator=(const MessageRouter&) = delete;

        ~MessageRouter();

        // Does nothing unless `layer`. Holds its own reference to the device's message queue, so it detaches without
        // the device.
        void attach(ID3D12Device12* device, bool layer);

        void detach();

      private:
        Com<ID3D12InfoQueue1> m_queue;
        DWORD m_cookie = 0;
    };

    [[nodiscard]] bool deviceRemoved(HRESULT result);

    // The reason, then the auto-breadcrumb nodes and the page fault when DRED is on.
    void reportRemoval(DeviceImpl& impl, std::string_view entry);

    Error failure(DeviceImpl& impl, HRESULT result, std::string_view what);
} // namespace rojoRHI::d3d12
