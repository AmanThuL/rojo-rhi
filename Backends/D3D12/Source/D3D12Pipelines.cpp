//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12Pipelines.cpp
/// @brief Implements the shader library and pipeline entries of the D3D12 backend.
//----------------------------------------------------------------------------------------------------------------------

#include <rojoRHI/RHI.h>

#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/Pipelines.h"
#include "Core/Records.h"
#include "D3D12Common.h"
#include "D3D12Debug.h"
#include "D3D12Device.h"
#include "D3D12PipelineState.h"
#include "D3D12ShaderLibrary.h"

#include <cstddef>
#include <format>
#include <initializer_list>
#include <memory>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace rojoRHI {
    namespace {
        //==============================================================================================================
        /// Returns the DXIL of `entry`, the entry of stage `stage`, or empty bytecode for an absent stage. The library's
        /// payload comes through `native`; its entries are searched under the pool's lock, since creation is not recording. A
        /// library that holds no such entry, which only a Release build lets through, is `DriverFailure`.
        Result<D3D12_SHADER_BYTECODE> bytecodeOf(core::DeviceCore& core, const ShaderEntry& entry, core::ShaderStage stage)
        {
            if (entry.library.bits == 0)
            {
                return D3D12_SHADER_BYTECODE{};
            }
            const auto* payload = static_cast<const d3d12::LibraryPayload*>(core.pool<ShaderLibrary>().native(entry.library));
            bool found = false;
            size_t index = 0;
            core.pool<ShaderLibrary>().update(entry.library, [&](const core::ShaderLibraryRecord& record) {
                for (size_t i = 0; i < record.entries.size(); ++i)
                {
                    if (record.entries[i].stage == stage && record.entries[i].name == entry.name)
                    {
                        found = true;
                        index = i;
                        return;
                    }
                }
            });
            if (payload == nullptr || !found)
            {
                return std::unexpected(
                    Error{ ErrorCategory::DriverFailure, std::format("the shader library holds no {} entry named \"{}\"", core::stageName(stage), entry.name) }
                );
            }
            const std::span<const std::byte> dxil = payload->bytecode(index);
            return D3D12_SHADER_BYTECODE{ dxil.data(), dxil.size() };
        }

        //==============================================================================================================
        /// Returns the DXIL of each stage `desc` names, or the error of the first stage its library does not hold.
        Result<d3d12::PipelineShaders> shadersOf(core::DeviceCore& core, const GraphicsPipelineDesc& desc)
        {
            const Result<D3D12_SHADER_BYTECODE> vertex = bytecodeOf(core, desc.vertex, core::ShaderStage::Vertex);
            const Result<D3D12_SHADER_BYTECODE> task = bytecodeOf(core, desc.task, core::ShaderStage::Task);
            const Result<D3D12_SHADER_BYTECODE> mesh = bytecodeOf(core, desc.mesh, core::ShaderStage::Mesh);
            const Result<D3D12_SHADER_BYTECODE> fragment = bytecodeOf(core, desc.fragment, core::ShaderStage::Fragment);
            for (const Result<D3D12_SHADER_BYTECODE>* resolved : { &vertex, &task, &mesh, &fragment })
            {
                if (!*resolved)
                {
                    return std::unexpected(resolved->error());
                }
            }
            return d3d12::PipelineShaders{ .vertex = *vertex, .task = *task, .mesh = *mesh, .fragment = *fragment };
        }

        //==============================================================================================================
        /// Creates the pipeline state `stream` describes on the calling thread, adding the compile time to the device's
        /// counter, names it with `record`'s label and publishes `state` with it in `record`. The pool counts the creation.
        Result<Pipeline> publish(
            DeviceImpl& impl, core::PipelineRecord&& record, const d3d12::PipelineStream& stream, std::unique_ptr<d3d12::PipelineState> state
        )
        {
            const D3D12_PIPELINE_STATE_STREAM_DESC desc = stream.desc();
            d3d12::Com<ID3D12PipelineState> pso;
            {
                const core::ScopedNanoseconds timer(impl.core.counts().pipelineCompileNanoseconds);
                if (const HRESULT created = impl.device()->CreatePipelineState(&desc, IID_PPV_ARGS(pso.put())); FAILED(created))
                {
                    return std::unexpected(d3d12::failure(impl, created, std::format("ID3D12Device2::CreatePipelineState for pipeline \"{}\"", record.label)));
                }
            }
            d3d12::setName(pso.get(), record.label);
            state->pso = pso.get();
            record.native = state.get();
            const Pipeline pipeline = impl.core.pool<Pipeline>().allocate(std::move(record));
            if (pipeline.bits == 0)
            {
                return std::unexpected(core::tableFull("pipeline", impl.core.pool<Pipeline>().capacity()));
            }
            // The record owns the payload and the pipeline state's reference now; the retire hook releases both.
            (void)pso.detach();
            (void)state.release();
            return pipeline;
        }
    } // namespace

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
    Result<Pipeline> Device::createPipeline(const GraphicsPipelineDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::PipelineRecord record = core::prepareGraphicsPipeline(impl.core, desc);
        const Result<d3d12::PipelineShaders> shaders = shadersOf(impl.core, desc);
        if (!shaders)
        {
            return std::unexpected(shaders.error());
        }
        auto state = std::make_unique<d3d12::PipelineState>();
        const bool mesh = desc.mesh.library.bits != 0;
        state->kind = mesh ? d3d12::PipelineKind::Mesh : d3d12::PipelineKind::Graphics;
        state->listTopology = mesh ? D3D_PRIMITIVE_TOPOLOGY_UNDEFINED : d3d12::listTopology(desc.primitive);
        state->depthBias = desc.raster.depthBias;
        return publish(impl, std::move(record), d3d12::graphicsStream(impl.roots.global(), desc, *shaders), std::move(state));
    }

    //==================================================================================================================
    Result<Pipeline> Device::createPipeline(const ComputePipelineDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::PipelineRecord record = core::prepareComputePipeline(impl.core, desc);
        const Result<D3D12_SHADER_BYTECODE> compute = bytecodeOf(impl.core, desc.compute, core::ShaderStage::Compute);
        if (!compute)
        {
            return std::unexpected(compute.error());
        }
        auto state = std::make_unique<d3d12::PipelineState>();
        state->kind = d3d12::PipelineKind::Compute;
        return publish(impl, std::move(record), d3d12::computeStream(impl.roots.global(), *compute), std::move(state));
    }

    //==================================================================================================================
    Result<std::vector<std::byte>> Device::pipelineCacheData()
    {
        return d3d12::unsupported("Device::pipelineCacheData");
    }
} // namespace rojoRHI
