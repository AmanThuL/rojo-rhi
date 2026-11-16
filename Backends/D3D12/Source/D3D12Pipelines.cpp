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
#include "D3D12Device.h"
#include "D3D12PipelineCache.h"
#include "D3D12PipelineKey.h"
#include "D3D12PipelineState.h"
#include "D3D12ShaderLibrary.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <initializer_list>
#include <memory>
#include <span>
#include <string>
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
        /// Returns the output topology the mesh entry `dxil` declares in its `PSV0` part, `DXIL::MeshOutputTopology`: 1 for
        /// lines, 2 for triangles, and 0 when the container holds no such part. The container header is "DXBC", a 16-byte
        /// digest, two 16-bit version fields, its size and its part count, then one offset per part; a part is its four
        /// characters and size, then its data. `PSV0`'s data starts with the size of its `PSVRuntimeInfo`, whose version 1
        /// holds the mesh topology at byte 27, after version 0's 24 bytes, the stage, the view-ID flag and the mesh
        /// shader's primitive-signature vectors.
        uint32_t meshOutputTopology(std::span<const std::byte> dxil)
        {
            const auto word = [&](size_t at) -> uint32_t {
                uint32_t value = 0;
                if (at + sizeof(value) <= dxil.size())
                {
                    std::memcpy(&value, dxil.data() + at, sizeof(value));
                }
                return value;
            };
            const auto fourCC = [](char a, char b, char c, char d) {
                return uint32_t{ uint8_t(a) } | uint32_t{ uint8_t(b) } << 8 | uint32_t{ uint8_t(c) } << 16 | uint32_t{ uint8_t(d) } << 24;
            };
            if (dxil.size() < 32 || word(0) != fourCC('D', 'X', 'B', 'C'))
            {
                return 0;
            }
            const uint32_t parts = word(28);
            for (uint32_t part = 0; part < parts; ++part)
            {
                const size_t offset = word(32 + size_t{ part } * 4);
                if (word(offset) != fourCC('P', 'S', 'V', '0'))
                {
                    continue;
                }
                const size_t info = offset + 8 + 4;
                return word(offset + 8) >= 28 && info + 27 < dxil.size() ? static_cast<uint32_t>(dxil[info + 27]) : 0;
            }
            return 0;
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
        /// Returns the bytes of `bytecode`, empty for an absent stage.
        std::span<const std::byte> dxilOf(const D3D12_SHADER_BYTECODE& bytecode)
        {
            return { static_cast<const std::byte*>(bytecode.pShaderBytecode), bytecode.BytecodeLength };
        }

        //==============================================================================================================
        /// Takes the pipeline state `stream` describes from the device's pipeline cache under the name of `canonical`, the
        /// key bytes of its description, or creates it on the calling thread, adding the time to the device's counter; names
        /// it with `record`'s label and publishes `state` with it in `record`. The pool counts the creation, and a pipeline the
        /// cache served counts a hit once it is published; the core counts every other creation as a miss.
        Result<Pipeline> publish(
            DeviceImpl& impl,
            core::PipelineRecord&& record,
            const d3d12::PipelineStream& stream,
            std::span<const std::byte> canonical,
            std::unique_ptr<d3d12::PipelineState> state
        )
        {
            const std::wstring name = d3d12::keyName(d3d12::pipelineKey(canonical));
            bool hit = false;
            d3d12::Com<ID3D12PipelineState> pso;
            {
                const core::ScopedNanoseconds timer(impl.core.counts().pipelineCompileNanoseconds);
                Result<ID3D12PipelineState*> created = impl.pipelineCache.create(impl, stream, name, record.label, hit);
                if (!created)
                {
                    return std::unexpected(std::move(created.error()));
                }
                pso = d3d12::Com<ID3D12PipelineState>(*created);
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
            if (hit)
            {
                // Counted once the pipeline exists, so a refused creation counts no hit.
                core::DeviceCounts::add(impl.core.counts().pipelineCacheHits);
            }
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
        const bool mesh = desc.mesh.library.bits != 0;
        // D3D12 rasterizes triangles alone conservatively and its debug layer rejects any other topology, which a mesh entry
        // declares in its DXIL rather than through `primitive`.
        if (mesh && desc.raster.conservative && meshOutputTopology(dxilOf(shaders->mesh)) != 2)
        {
            return std::unexpected(
                Error{ ErrorCategory::Unsupported,
                       std::format("conservative rasterization needs a mesh entry that emits triangles, and \"{}\" does not", desc.mesh.name) }
            );
        }
        auto state = std::make_unique<d3d12::PipelineState>();
        state->kind = mesh ? d3d12::PipelineKind::Mesh : d3d12::PipelineKind::Graphics;
        state->listTopology = mesh ? D3D_PRIMITIVE_TOPOLOGY_UNDEFINED : d3d12::listTopology(desc.primitive);
        state->depthBias = desc.raster.depthBias;
        state->readsStencilReference = desc.depthStencil.stencilTest;
        for (const ColorTarget& color : desc.colors)
        {
            const auto constant = [](BlendFactor factor) { return factor == BlendFactor::Constant || factor == BlendFactor::OneMinusConstant; };
            const BlendState& blend = color.blend;
            state->readsBlendFactor =
                state->readsBlendFactor ||
                (blend.enable && (constant(blend.srcColor) || constant(blend.dstColor) || constant(blend.srcAlpha) || constant(blend.dstAlpha)));
        }
        const std::vector<std::byte> key = d3d12::canonicalKey(
            desc,
            d3d12::StageDxil{
                .vertex = dxilOf(shaders->vertex), .task = dxilOf(shaders->task), .mesh = dxilOf(shaders->mesh), .fragment = dxilOf(shaders->fragment) }
        );
        return publish(impl, std::move(record), d3d12::graphicsStream(impl.roots.global(), desc, *shaders), key, std::move(state));
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
        const std::vector<std::byte> key = d3d12::canonicalKey(desc, dxilOf(*compute));
        return publish(impl, std::move(record), d3d12::computeStream(impl.roots.global(), *compute), key, std::move(state));
    }

    //==================================================================================================================
    Result<std::vector<std::byte>> Device::pipelineCacheData()
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        return impl.pipelineCache.data(impl);
    }
} // namespace rojoRHI
