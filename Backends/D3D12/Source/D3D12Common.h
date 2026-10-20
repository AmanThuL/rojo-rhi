//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Common.h
/// @brief Declares the helpers every D3D12 backend file shares: the refusal and the abort of an entry the backend does not implement.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <expected>
#include <string_view>

namespace rojoRHI::d3d12 {
    /// Logs that the D3D12 backend does not implement `entry`, named `Class::member`, then aborts, in every build.
    [[noreturn]] void notImplemented(std::string_view entry);

    /// Returns the `Unsupported` error an entry that returns a `Result` gives, naming `entry` as `Class::member`.
    std::unexpected<Error> unsupported(std::string_view entry);
} // namespace rojoRHI::d3d12
