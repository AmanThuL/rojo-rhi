//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Loader.h
/// @brief Declares how a D3D12 device is opened: the OS check, the Agility device factory, debug features and the adapter.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"

#include <dxgi1_6.h>

namespace rojoRHI::d3d12 {
    // Each device opens its own, so devices never share a factory or a debug configuration. Members are released in
    // reverse order, the device first.
    struct Loaded
    {
        Com<ID3D12DeviceFactory> factory; // Holds the debug configuration.
        Com<IDXGIFactory6> dxgi;
        Com<IDXGIAdapter1> adapter;
        Com<ID3D12Device12> device; // At feature level 12_2, past the floor check.
        bool layer = false;         // The debug layer, and DRED with it, enabled on the factory.
        bool gpuValidation = false;
    };

    // In the order the backend requires: the Windows 11 build check, the Agility device factory in `D3D12\` beside the
    // executable, the build's debug features, then each candidate adapter until one creates a device at feature level
    // 12_2 that passes `checkFloor`. Returns `Unsupported` naming the first missing requirement; whatever was opened
    // before a failure is released.
    Result<Loaded> loadDevice();
} // namespace rojoRHI::d3d12
