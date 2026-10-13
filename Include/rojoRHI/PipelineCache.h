//----------------------------------------------------------------------------------------------------------------------
/// @file PipelineCache.h
/// @brief Declares the opaque pipeline cache blob.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <span>

namespace rojoRHI {
    /// An opaque blob of compiled pipelines: the bytes `Device::pipelineCacheData` returns, passed back through
    /// `DeviceDesc::pipelineCache`. An empty span means no cache. A blob from another backend, device, driver or RojoRHI
    /// version is discarded as a cache miss and reported through the message callback; it is never an error. The bytes are read
    /// during `createDevice`, and the caller keeps them valid and unchanged until it returns.
    using PipelineCache = std::span<const std::byte>;
} // namespace rojoRHI
