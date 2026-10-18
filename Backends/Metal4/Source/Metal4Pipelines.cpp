//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Pipelines.cpp
/// @brief Implements the public shader library and pipeline entries for the Metal 4 backend.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Device.h"

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace rojoRHI {

    //==================================================================================================================
    Result<ShaderLibrary> Device::loadShaderLibrary([[maybe_unused]] std::span<const std::byte> blob, [[maybe_unused]] std::string_view label)
    {
        DeviceImpl::of(*this);
        return std::unexpected(Error{ ErrorCategory::Unsupported, "Device::loadShaderLibrary is not supported by the Metal 4 backend" });
    }

    //==================================================================================================================
    Result<Pipeline> Device::createPipeline([[maybe_unused]] const GraphicsPipelineDesc& desc)
    {
        DeviceImpl::of(*this);
        return std::unexpected(Error{ ErrorCategory::Unsupported, "Device::createPipeline(GraphicsPipelineDesc) is not supported by the Metal 4 backend" });
    }

    //==================================================================================================================
    Result<Pipeline> Device::createPipeline([[maybe_unused]] const ComputePipelineDesc& desc)
    {
        DeviceImpl::of(*this);
        return std::unexpected(Error{ ErrorCategory::Unsupported, "Device::createPipeline(ComputePipelineDesc) is not supported by the Metal 4 backend" });
    }

    //==================================================================================================================
    Result<std::vector<std::byte>> Device::pipelineCacheData()
    {
        DeviceImpl::of(*this);
        return std::unexpected(Error{ ErrorCategory::Unsupported, "Device::pipelineCacheData is not supported by the Metal 4 backend" });
    }

    //==================================================================================================================
    void Device::destroy(Pipeline pipeline)
    {
        DeviceImpl::of(*this).core.destroy(pipeline);
    }

    //==================================================================================================================
    void Device::destroy(ShaderLibrary library)
    {
        DeviceImpl::of(*this).core.destroy(library);
    }
} // namespace rojoRHI
