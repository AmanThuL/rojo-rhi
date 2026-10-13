//----------------------------------------------------------------------------------------------------------------------
/// @file Result.h
/// @brief Declares the creation result type and the error it carries.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <expected>
#include <string>

namespace rojoRHI {
    /// The stable failure category a caller branches on; enumerators are only ever appended.
    enum class ErrorCategory : uint8_t
    {
        Unsupported,       ///< The hardware, driver or build lacks a required feature.
        OutOfMemory,       ///< A device or host allocation failed.
        DriverFailure,     ///< The driver refused a well-formed request.
        ShaderAbiMismatch, ///< A shader library carries another ROJORHI_SHADER_ABI_VERSION.
    };

    /// A failure of a creation entry. It owns its message string and is not trivially copyable.
    struct Error
    {
        /// Stable failure category; stable across versions.
        ErrorCategory category;
        /// Human-readable context, including the native error text; its wording may change in any release.
        std::string message;
    };

    /// The result of a creation entry: the created value or an Error. `Result<void>` carries no value. An entry that
    /// returns an error has created nothing and leaked nothing.
    template<class T> using Result = std::expected<T, Error>;
} // namespace rojoRHI
