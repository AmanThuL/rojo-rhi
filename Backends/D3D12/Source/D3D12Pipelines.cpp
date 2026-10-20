//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Pipelines.cpp
/// @brief Implements the shader library and pipeline entries of the D3D12 backend.
//----------------------------------------------------------------------------------------------------------------------

#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Pipelines.h"
#include "D3D12Common.h"
#include "D3D12Device.h"
#include "D3D12ShaderLibrary.h"

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace rojoRHI {
    //==================================================================================================================
    Result<ShaderLibrary> Device::loadShaderLibrary(std::span<const std::byte> blob, std::string_view label)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::ShaderLibraryRecord record = core::prepareShaderLibrary(blob, label);
        const Result<d3d12::LibraryHeader> header = d3d12::parseShaderLibrary(blob);
        if (!header)
        {
            return std::unexpected(header.error());
        }
        auto payload = std::make_unique<d3d12::LibraryPayload>();
        for (const d3d12::LibraryEntry& entry : header->entries)
        {
            record.entries.push_back({ entry.name, entry.stage });
            payload->ranges.emplace_back(payload->dxil.size(), entry.dxil.size());
            payload->dxil.insert(payload->dxil.end(), entry.dxil.begin(), entry.dxil.end());
        }
        record.native = payload.get();
        const ShaderLibrary library = impl.core.pool<ShaderLibrary>().allocate(std::move(record));
        if (library.bits == 0)
        {
            return std::unexpected(core::tableFull("shader library", impl.core.pool<ShaderLibrary>().capacity()));
        }
        payload.release();
        return library;
    }

    //==================================================================================================================
    Result<Pipeline> Device::createPipeline(const GraphicsPipelineDesc&)
    {
        return d3d12::unsupported("Device::createPipeline(GraphicsPipelineDesc)");
    }

    //==================================================================================================================
    Result<Pipeline> Device::createPipeline(const ComputePipelineDesc&)
    {
        return d3d12::unsupported("Device::createPipeline(ComputePipelineDesc)");
    }

    //==================================================================================================================
    Result<std::vector<std::byte>> Device::pipelineCacheData()
    {
        return d3d12::unsupported("Device::pipelineCacheData");
    }
} // namespace rojoRHI
