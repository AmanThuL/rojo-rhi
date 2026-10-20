//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Backend.cpp
/// @brief Implements the backend-dependent test support for the D3D12 backend: hidden windows as surfaces, and no shader library.
//----------------------------------------------------------------------------------------------------------------------

#include "Support/Backend.h"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <vector>

namespace rojoRHI::test {
    namespace {
        /// The surfaces a test may ask for, each a distinct window.
        constexpr uint32_t kSurfaceCount = 8;

        /// The width and height of every surface window, in pixels.
        constexpr int kSurfaceSize = 256;

        /// The window class every surface window belongs to, registered once for the process.
        constexpr const wchar_t* kSurfaceClass = L"RojoRHITestSurface";
    } // namespace

    //==================================================================================================================
    // Fails the running case, which ends it, as every library entry here does: the D3D12 backend loads no shader library.
    std::vector<std::byte> standardLibrary()
    {
        FAIL("the D3D12 backend loads no shader library, so the test support has no standard library");
    }

    //==================================================================================================================
    std::vector<std::byte> libraryStampedWith(uint32_t)
    {
        FAIL("the D3D12 backend loads no shader library, so the test support has no stamped library");
    }

    //==================================================================================================================
    std::vector<std::byte> foreignLibrary()
    {
        FAIL("the D3D12 backend loads no shader library, so the test support has no foreign library");
    }

    //==================================================================================================================
    std::vector<std::byte> shaderLibraryFile(std::string_view)
    {
        FAIL("the D3D12 backend loads no shader library, so the test support has no shader library file");
    }

    //==================================================================================================================
    // Each surface is a hidden 256 x 256 window, never shown, created on first use and alive for the process.
    void* surface(uint32_t index)
    {
        if (index >= kSurfaceCount)
        {
            FAIL("the D3D12 test support has surfaces 0 to 7, and the case asked for surface " << index);
        }
        static std::mutex mutex;
        static bool registered = false;
        static std::array<HWND, kSurfaceCount> windows = {};
        const std::lock_guard lock(mutex);
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        if (!registered)
        {
            WNDCLASSEXW windowClass = {};
            windowClass.cbSize = static_cast<UINT>(sizeof(windowClass));
            windowClass.lpfnWndProc = DefWindowProcW;
            windowClass.hInstance = instance;
            windowClass.lpszClassName = kSurfaceClass;
            if (RegisterClassExW(&windowClass) == 0)
            {
                FAIL("RegisterClassExW for the test surfaces failed with error " << GetLastError());
            }
            registered = true;
        }
        if (windows[index] == nullptr)
        {
            windows[index] = CreateWindowExW(
                0,
                kSurfaceClass,
                L"RojoRHI test surface",
                WS_OVERLAPPEDWINDOW,
                CW_USEDEFAULT,
                CW_USEDEFAULT,
                kSurfaceSize,
                kSurfaceSize,
                nullptr,
                nullptr,
                instance,
                nullptr
            );
            if (windows[index] == nullptr)
            {
                FAIL("CreateWindowExW for test surface " << index << " failed with error " << GetLastError());
            }
        }
        return windows[index];
    }

    //==================================================================================================================
    bool runsOnGpu()
    {
        return true;
    }
} // namespace rojoRHI::test
