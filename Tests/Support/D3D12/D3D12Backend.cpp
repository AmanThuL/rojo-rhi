//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Backend.cpp
/// @brief Implements the D3D12 test support: hidden windows as surfaces and the packaged libraries.
//----------------------------------------------------------------------------------------------------------------------

#include "Support/Backend.h"

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
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
        //==============================================================================================================
        /// Reads the library file `path`, failing the running case when it is missing or empty.
        std::vector<std::byte> readLibrary(const std::filesystem::path& path)
        {
            std::ifstream file(path, std::ios::binary);
            if (!file)
            {
                FAIL("the shader library " << std::filesystem::absolute(path) << " is missing");
            }
            const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (text.empty())
            {
                FAIL("the shader library " << std::filesystem::absolute(path) << " is empty");
            }
            const auto* begin = reinterpret_cast<const std::byte*>(text.data());
            return { begin, begin + text.size() };
        }

        //==============================================================================================================
        /// The packaged standard library, read on first use and kept for the process.
        const std::vector<std::byte>& standardBytes()
        {
            static const std::vector<std::byte> bytes = readLibrary("Shaders/Standard.d3d12lib");
            return bytes;
        }
    } // namespace

    //==================================================================================================================
    std::vector<std::byte> standardLibrary()
    {
        return standardBytes();
    }

    //==================================================================================================================
    // The container's second line is `abi <N>`; replacing it leaves the DXIL byte counts unchanged.
    std::vector<std::byte> libraryStampedWith(uint32_t abi)
    {
        std::vector<std::byte> bytes = standardBytes();
        const auto newline = [&](size_t from) {
            return std::find(bytes.begin() + static_cast<std::ptrdiff_t>(from), bytes.end(), std::byte{ '\n' }) - bytes.begin();
        };
        const size_t start = static_cast<size_t>(newline(0)) + 1;
        const size_t end = static_cast<size_t>(newline(start));
        REQUIRE(end < bytes.size());
        const std::string line = "abi " + std::to_string(abi);
        const auto* text = reinterpret_cast<const std::byte*>(line.data());
        bytes.erase(bytes.begin() + static_cast<std::ptrdiff_t>(start), bytes.begin() + static_cast<std::ptrdiff_t>(end));
        bytes.insert(bytes.begin() + static_cast<std::ptrdiff_t>(start), text, text + line.size());
        return bytes;
    }

    //==================================================================================================================
    std::vector<std::byte> foreignLibrary()
    {
        const std::string_view text = "rojorhi.null.shaderlibrary\nabi 1\nentry vsMain vertex\n";
        const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
        return { bytes, bytes + text.size() };
    }

    //==================================================================================================================
    // Fails the running case through Catch2, so it is called from a test thread.
    std::vector<std::byte> shaderLibraryFile(std::string_view name)
    {
        return readLibrary(std::filesystem::path("Shaders") / (std::string(name) + ".d3d12lib"));
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
