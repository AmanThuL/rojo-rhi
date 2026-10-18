//----------------------------------------------------------------------------------------------------------------------
/// @file ExecutableDirectory.h
/// @brief Declares the lookup of the directory holding the running executable, beside which its shaders are built.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <expected>
#include <filesystem>
#include <string>

namespace rojoRHI::bench {
    // The canonical directory of the running executable, from _NSGetExecutablePath.
    std::expected<std::filesystem::path, std::string> executableDirectory();
} // namespace rojoRHI::bench
