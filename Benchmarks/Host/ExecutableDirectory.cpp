//----------------------------------------------------------------------------------------------------------------------
/// @file ExecutableDirectory.cpp
/// @brief Implements the executable directory lookup through dyld's _NSGetExecutablePath.
//----------------------------------------------------------------------------------------------------------------------
#include "Host/ExecutableDirectory.h"

#include <mach-o/dyld.h>

#include <cstdint>
#include <system_error>
#include <vector>

namespace rojoRHI::bench {
    //==================================================================================================================
    std::expected<std::filesystem::path, std::string> executableDirectory()
    {
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::vector<char> path(size + 1, '\0');
        if (_NSGetExecutablePath(path.data(), &size) != 0)
        {
            return std::unexpected(std::string("the executable path does not fit the buffer _NSGetExecutablePath sized"));
        }
        std::error_code error;
        const std::filesystem::path canonical = std::filesystem::canonical(path.data(), error);
        if (error)
        {
            return std::unexpected("cannot resolve the executable path " + std::string(path.data()) + ": " + error.message());
        }
        return canonical.parent_path();
    }
} // namespace rojoRHI::bench
