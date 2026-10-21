//----------------------------------------------------------------------------------------------------------------------
/// @file ExecutableDirectory.cpp
/// @brief Implements the executable directory lookup through GetModuleFileNameW.
//----------------------------------------------------------------------------------------------------------------------
#include "Host/ExecutableDirectory.h"

#include <windows.h>

#include <format>
#include <system_error>

namespace rojoRHI::bench {
    //==================================================================================================================
    // The buffer doubles until the path fits: GetModuleFileNameW truncates a longer path and returns the buffer's size.
    std::expected<std::filesystem::path, std::string> executableDirectory()
    {
        std::wstring path(MAX_PATH, L'\0');
        for (;;)
        {
            const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (length == 0)
            {
                return std::unexpected(std::format("GetModuleFileNameW failed with error {}", GetLastError()));
            }
            if (length < path.size())
            {
                path.resize(length);
                break;
            }
            path.resize(path.size() * 2);
        }
        std::error_code error;
        const std::filesystem::path canonical = std::filesystem::canonical(std::filesystem::path(path), error);
        if (error)
        {
            return std::unexpected("cannot resolve the executable path " + std::filesystem::path(path).string() + ": " + error.message());
        }
        return canonical.parent_path();
    }
} // namespace rojoRHI::bench
