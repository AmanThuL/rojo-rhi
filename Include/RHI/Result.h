//----------------------------------------------------------------------------------------------------------------------
/// @file Result.h
/// @brief Defines the RHI error domain and expected-based result type.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <expected>
#include <string>

namespace lmx::rhi {

/// Identifies the category of an RHI operation failure.
enum class ErrorCode {
    DeviceUnsupported,      ///< The requested device or required GPU family is unavailable.
    ShaderLoadFailed,       ///< A shader library could not be loaded or compiled.
    PipelineCreationFailed, ///< A graphics pipeline could not be created.
    ResourceCreationFailed, ///< A buffer, texture, or sampler could not be created.
    SwapchainFailed,        ///< Presentation setup or drawable acquisition failed.
    InvalidDesc,            ///< A descriptor violated an RHI precondition.
};

/// Describes an RHI failure with a stable category and diagnostic message.
struct Error {
    ErrorCode code;      ///< Stable failure category.
    std::string message; ///< Human-readable diagnostic context.
};

/// Holds either an RHI operation value or an RHI error.
template <typename T>
using Result = std::expected<T, Error>;

} // namespace lmx::rhi
