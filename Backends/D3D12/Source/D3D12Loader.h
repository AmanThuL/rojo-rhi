//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Loader.h
/// @brief Declares how a D3D12 device is opened: the OS check, the Agility device factory, debug features and the adapter.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "D3D12Common.h"

#include <dxgi1_6.h>

namespace rojoRHI::d3d12 {
    /// What `loadDevice` opened for one device: the device factory of the Agility runtime beside the executable, the DXGI
    /// factory, the adapter and the device on it. Each device owns its own, so devices never share a factory or a debug
    /// configuration. Members are released in reverse order, the device first.
    struct Loaded
    {
        Com<ID3D12DeviceFactory> factory; ///< The Agility runtime's device factory, which holds the debug configuration.
        Com<IDXGIFactory6> dxgi;          ///< The DXGI factory the adapter came from.
        Com<IDXGIAdapter1> adapter;       ///< The adapter the device runs on.
        Com<ID3D12Device12> device;       ///< The device, at feature level 12_2, past the floor check.
        bool layer = false;               ///< True when the build enabled the debug layer (and DRED) on the factory.
        bool gpuValidation = false;       ///< True when the build also enabled GPU-based validation.
    };

    /// Opens a device in the order the backend requires: the Windows 11 build check, the Agility device factory on the
    /// `D3D12\` directory beside the executable, the debug features the build selects, then each candidate adapter of the
    /// configured kind until one creates a device at feature level 12_2 that passes `checkFloor`. Returns `Unsupported` naming
    /// the first missing requirement; whatever it opened before a failure is released.
    Result<Loaded> loadDevice();
} // namespace rojoRHI::d3d12
