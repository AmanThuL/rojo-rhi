//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12ProbeExports.cpp
/// @brief Exports the Agility SDK version and path, so the D3D12 runtime loads D3D12Core.dll from D3D12\ beside the executable.
//----------------------------------------------------------------------------------------------------------------------

#include <windows.h>

extern "C"
{
    __declspec(dllexport) extern const UINT D3D12SDKVersion = 619;
    __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
