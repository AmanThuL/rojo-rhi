//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Debug.cpp
/// @brief Stubs the D3D12 debug-layer message routing, which routes nothing yet, and the report of a removed device.
//----------------------------------------------------------------------------------------------------------------------

#include "D3D12Debug.h"

#include "Base/Log.h"
#include "D3D12Common.h"

#include <dxgi1_6.h>

#include <string_view>

namespace rojoRHI::d3d12 {
    //==================================================================================================================
    void MessageRouter::attach(ID3D12Device12*, bool)
    {
    }

    //==================================================================================================================
    void MessageRouter::detach()
    {
    }

    //==================================================================================================================
    bool deviceRemoved(HRESULT result)
    {
        return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET || result == DXGI_ERROR_DEVICE_HUNG;
    }

    //==================================================================================================================
    void reportRemoval(DeviceImpl&, std::string_view entry)
    {
        ROJORHI_LOG_ERROR("{} found the D3D12 device removed", entry);
    }

    //==================================================================================================================
    Error failure(DeviceImpl&, HRESULT result, std::string_view what)
    {
        return hresultError(result, what);
    }
} // namespace rojoRHI::d3d12
