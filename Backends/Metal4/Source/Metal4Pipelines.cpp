//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Pipelines.cpp
/// @brief Implements the public shader library and pipeline entries for the Metal 4 backend.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Device.h"

#include <rojoRHI/RHI.h>

#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "Core/Pipelines.h"
#include "Core/Records.h"
#include "Metal4Common.h"
#include "Metal4Formats.h"
#include "Metal4PipelineState.h"
#include "Metal4ShaderLibrary.h"

#include <Metal/Metal.hpp>
#include <dispatch/dispatch.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <initializer_list>
#include <memory>
#include <shared_mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace rojoRHI {
    namespace {
        //==============================================================================================================
        /// Returns a new function descriptor (+1) for `entry`, or null for an absent stage, and writes the entry's threadgroup
        /// size to `size` when it is given. The library's payload comes through `native`; its entries are read under the pool's
        /// lock, because a record outlives no lookup in a Release build.
        MTL4::LibraryFunctionDescriptor* newFunction(core::DeviceCore& core, const ShaderEntry& entry, core::ShaderStage stage, MTL::Size* size)
        {
            if (entry.library.bits == 0)
            {
                return nullptr;
            }
            const auto* payload = static_cast<const metal4::LibraryPayload*>(core.pool<ShaderLibrary>().native(entry.library));
            if (size != nullptr)
            {
                std::array<uint32_t, 3> threads = {};
                core.pool<ShaderLibrary>().update(entry.library, [&](const core::ShaderLibraryRecord& record) {
                    for (size_t i = 0; i < record.entries.size(); ++i)
                    {
                        if (record.entries[i].stage == stage && record.entries[i].name == entry.name)
                        {
                            threads = payload->sizes[i];
                            break;
                        }
                    }
                });
                *size = MTL::Size(threads[0], threads[1], threads[2]);
            }
            MTL4::LibraryFunctionDescriptor* function = MTL4::LibraryFunctionDescriptor::alloc()->init();
            function->setLibrary(payload->library);
            function->setName(metal4::makeString(entry.name));
            return function;
        }

        //==============================================================================================================
        /// Releases each descriptor of `functions` that is not null.
        void releaseFunctions(std::initializer_list<MTL4::LibraryFunctionDescriptor*> functions)
        {
            for (MTL4::LibraryFunctionDescriptor* function : functions)
            {
                if (function != nullptr)
                {
                    function->release();
                }
            }
        }

        //==============================================================================================================
        /// True when a graphics pipeline is first compiled with rasterization on: nothing it rasterizes is observable without
        /// a fragment entry or a depth attachment, and a vertex entry that returns nothing needs rasterization off.
        bool rasterizes(const GraphicsPipelineDesc& desc)
        {
            return desc.fragment.library.bits != 0 || desc.depthFormat != Format::Undefined;
        }

        //==============================================================================================================
        /// Compiles a vertex pipeline from `descriptor`, adding the probe and compile time to the device's counter, and sets
        /// `hit` when the device's pipeline cache holds the pipeline. Every pipeline compiles through the compiler, which
        /// serves a cached one from the archive, so the serializer records each pipeline, hit or miss.
        MTL::RenderPipelineState* compileRender(DeviceImpl& impl, const MTL4::RenderPipelineDescriptor* descriptor, NS::Error** error, bool& hit)
        {
            const core::ScopedNanoseconds timer(impl.core.counts().pipelineCompileNanoseconds);
            const MTL4::CompilerTaskOptions* options = impl.pipelineCache.options();
            hit = options != nullptr && impl.pipelineCache.probe(descriptor);
            *error = nullptr;
            const std::shared_lock compiling = impl.pipelineCache.compiling();
            return impl.compiler->newRenderPipelineState(descriptor, options, error);
        }

        //==============================================================================================================
        /// Compiles a mesh pipeline from `descriptor` as `compileRender` compiles a vertex pipeline.
        MTL::RenderPipelineState* compileMesh(DeviceImpl& impl, const MTL4::MeshRenderPipelineDescriptor* descriptor, NS::Error** error, bool& hit)
        {
            const core::ScopedNanoseconds timer(impl.core.counts().pipelineCompileNanoseconds);
            const MTL4::CompilerTaskOptions* options = impl.pipelineCache.options();
            hit = options != nullptr && impl.pipelineCache.probe(descriptor);
            *error = nullptr;
            const std::shared_lock compiling = impl.pipelineCache.compiling();
            return impl.compiler->newRenderPipelineState(descriptor, options, error);
        }

        //==============================================================================================================
        /// Compiles a compute pipeline from `descriptor` as `compileRender` compiles a vertex pipeline.
        MTL::ComputePipelineState* compileCompute(DeviceImpl& impl, const MTL4::ComputePipelineDescriptor* descriptor, NS::Error** error, bool& hit)
        {
            const core::ScopedNanoseconds timer(impl.core.counts().pipelineCompileNanoseconds);
            const MTL4::CompilerTaskOptions* options = impl.pipelineCache.options();
            hit = options != nullptr && impl.pipelineCache.probe(descriptor);
            *error = nullptr;
            const std::shared_lock compiling = impl.pipelineCache.compiling();
            return impl.compiler->newComputePipelineState(descriptor, options, error);
        }

        //==============================================================================================================
        /// Builds and compiles the vertex pipeline `desc` labelled `label` into `state.render`, sets `hit` when the pipeline
        /// cache held the descriptor it was compiled from, and returns why Metal refused it, or an empty string. The caller
        /// holds a pool.
        std::string buildVertex(DeviceImpl& impl, const GraphicsPipelineDesc& desc, const std::string& label, metal4::PipelineState& state, bool& hit)
        {
            MTL4::LibraryFunctionDescriptor* vertex = newFunction(impl.core, desc.vertex, core::ShaderStage::Vertex, nullptr);
            MTL4::LibraryFunctionDescriptor* fragment = newFunction(impl.core, desc.fragment, core::ShaderStage::Fragment, nullptr);
            MTL4::RenderPipelineDescriptor* descriptor = MTL4::RenderPipelineDescriptor::alloc()->init();
            descriptor->setVertexFunctionDescriptor(vertex);
            descriptor->setFragmentFunctionDescriptor(fragment);
            metal4::describeColors(descriptor->colorAttachments(), desc.colors);
            descriptor->setRasterSampleCount(desc.samples);
            descriptor->setInputPrimitiveTopology(metal4::topologyClass(desc.primitive));
            descriptor->setSupportIndirectCommandBuffers(MTL4::IndirectCommandBufferSupportStateEnabled);
            const bool rasterize = rasterizes(desc);
            descriptor->setRasterizationEnabled(rasterize);
            descriptor->setLabel(metal4::makeString(label));
            NS::Error* error = nullptr;
            state.render = compileRender(impl, descriptor, &error, hit);
            std::string failure = state.render == nullptr ? metal4::describe(error) : std::string();
            if (state.render == nullptr && fragment == nullptr)
            {
                // Metal runs a vertex entry without rasterization only when it returns nothing, and with it only when it
                // returns a position; the library does not say which, so a refused pipeline without a fragment entry is
                // compiled once more with the other setting, and both refusals are reported when it fails again.
                NS::Error* retryError = nullptr;
                descriptor->setRasterizationEnabled(!rasterize);
                state.render = compileRender(impl, descriptor, &retryError, hit);
                failure = state.render != nullptr ? std::string()
                                                  : std::format(
                                                        "with rasterization {}: {}; with rasterization {}: {}",
                                                        rasterize ? "on" : "off",
                                                        failure,
                                                        rasterize ? "off" : "on",
                                                        metal4::describe(retryError)
                                                    );
            }
            descriptor->release();
            releaseFunctions({ vertex, fragment });
            return failure;
        }

        //==============================================================================================================
        /// Builds and compiles the mesh pipeline `desc` labelled `label` into `state.render`, with the object and mesh
        /// threadgroup sizes of its libraries, sets `hit` when the pipeline cache held it, and returns why Metal refused it, or
        /// an empty string. The caller holds a pool.
        std::string buildMesh(DeviceImpl& impl, const GraphicsPipelineDesc& desc, const std::string& label, metal4::PipelineState& state, bool& hit)
        {
            MTL4::LibraryFunctionDescriptor* object = newFunction(impl.core, desc.task, core::ShaderStage::Task, &state.objectThreads);
            MTL4::LibraryFunctionDescriptor* mesh = newFunction(impl.core, desc.mesh, core::ShaderStage::Mesh, &state.meshThreads);
            MTL4::LibraryFunctionDescriptor* fragment = newFunction(impl.core, desc.fragment, core::ShaderStage::Fragment, nullptr);
            MTL4::MeshRenderPipelineDescriptor* descriptor = MTL4::MeshRenderPipelineDescriptor::alloc()->init();
            descriptor->setObjectFunctionDescriptor(object);
            descriptor->setMeshFunctionDescriptor(mesh);
            descriptor->setFragmentFunctionDescriptor(fragment);
            if (object != nullptr)
            {
                descriptor->setRequiredThreadsPerObjectThreadgroup(state.objectThreads);
            }
            descriptor->setRequiredThreadsPerMeshThreadgroup(state.meshThreads);
            metal4::describeColors(descriptor->colorAttachments(), desc.colors);
            descriptor->setRasterSampleCount(desc.samples);
            descriptor->setSupportIndirectCommandBuffers(MTL4::IndirectCommandBufferSupportStateEnabled);
            descriptor->setRasterizationEnabled(rasterizes(desc));
            descriptor->setLabel(metal4::makeString(label));
            NS::Error* error = nullptr;
            state.render = compileMesh(impl, descriptor, &error, hit);
            const std::string failure = state.render == nullptr ? metal4::describe(error) : std::string();
            descriptor->release();
            releaseFunctions({ object, mesh, fragment });
            return failure;
        }
    } // namespace

    //==================================================================================================================
    Result<ShaderLibrary> Device::loadShaderLibrary(std::span<const std::byte> blob, std::string_view label)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::ShaderLibraryRecord record = core::prepareShaderLibrary(blob, label);
        Result<metal4::LibraryHeader> header = metal4::parseShaderLibrary(blob);
        if (!header)
        {
            return std::unexpected(std::move(header.error()));
        }
        const metal4::AutoreleasePool pool;
        // The data object copies the bytes, which are consumed before return.
        const dispatch_data_t data = dispatch_data_create(header->metallib.data(), header->metallib.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NS::Error* error = nullptr;
        MTL::Library* native = impl.device->newLibrary(data, &error);
        dispatch_release(data);
        if (native == nullptr)
        {
            return std::unexpected(
                Error{ ErrorCategory::DriverFailure, std::format("Metal refused shader library \"{}\": {}", record.label, metal4::describe(error)) }
            );
        }
        auto payload = std::make_unique<metal4::LibraryPayload>();
        payload->library = native;
        for (const metal4::LibraryEntry& entry : header->entries)
        {
            MTL::FunctionType expected = MTL::FunctionTypeKernel;
            switch (entry.stage)
            {
            case core::ShaderStage::Vertex: expected = MTL::FunctionTypeVertex; break;
            case core::ShaderStage::Fragment: expected = MTL::FunctionTypeFragment; break;
            case core::ShaderStage::Task: expected = MTL::FunctionTypeObject; break;
            case core::ShaderStage::Mesh: expected = MTL::FunctionTypeMesh; break;
            case core::ShaderStage::Compute: expected = MTL::FunctionTypeKernel; break;
            }
            MTL::Function* function = native->newFunction(metal4::makeString(entry.name));
            if (function == nullptr)
            {
                payload->release();
                return std::unexpected(
                    Error{ ErrorCategory::DriverFailure, std::format("shader library \"{}\" does not hold the entry \"{}\"", record.label, entry.name) }
                );
            }
            const MTL::FunctionType actual = function->functionType();
            function->release();
            if (actual != expected)
            {
                payload->release();
                return std::unexpected(
                    Error{ ErrorCategory::DriverFailure,
                           std::format(
                               "entry \"{}\" of shader library \"{}\" is declared {} (Metal function type {}), but the function has Metal function type {}",
                               entry.name,
                               record.label,
                               core::stageName(entry.stage),
                               static_cast<int>(expected),
                               static_cast<int>(actual)
                           ) }
                );
            }
            record.entries.push_back({ entry.name, entry.stage });
            payload->sizes.push_back(entry.size);
        }
        native->setLabel(metal4::makeString(record.label));
        record.native = payload.get();
        const ShaderLibrary library = impl.core.pool<ShaderLibrary>().allocate(std::move(record));
        if (library.bits == 0)
        {
            payload->release();
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
        const metal4::AutoreleasePool pool;
        auto state = std::make_unique<metal4::PipelineState>();
        state->kind = core::PipelineKind::Graphics;
        state->mesh = desc.mesh.library.bits != 0;
        if (state->mesh && desc.fragment.library.bits == 0 && !desc.colors.empty() && rasterizes(desc))
        {
            // Metal's validation aborts on a rasterizing mesh pipeline with color targets and no fragment function.
            return std::unexpected(
                Error{ ErrorCategory::DriverFailure,
                       std::format(
                           "the pipeline \"{}\" was not created: a Metal 4 mesh pipeline with color targets and a depth format needs a fragment entry",
                           record.label
                       ) }
            );
        }
        bool hit = false;
        const std::string failure = state->mesh ? buildMesh(impl, desc, record.label, *state, hit) : buildVertex(impl, desc, record.label, *state, hit);
        if (state->render == nullptr)
        {
            return std::unexpected(Error{ ErrorCategory::DriverFailure, std::format("the pipeline \"{}\" was not created: {}", record.label, failure) });
        }
        if (metal4::testsDepthOrStencil(desc.depthStencil))
        {
            state->depthStencil = impl.depthStencils.get(impl.device, desc.depthStencil, impl.core.counts());
            if (state->depthStencil == nullptr)
            {
                state->release();
                return std::unexpected(
                    Error{ ErrorCategory::DriverFailure, std::format("the depth-stencil state of pipeline \"{}\" was not created", record.label) }
                );
            }
        }
        state->cull = metal4::cullMode(desc.raster.cull);
        state->winding = metal4::winding(desc.raster.frontFace);
        state->setsWinding = desc.raster.cull != CullMode::None || desc.depthStencil.stencilTest || desc.fragment.library.bits != 0;
        state->fill = metal4::triangleFillMode(desc.raster.fill);
        state->depthClip = desc.raster.depthClip ? MTL::DepthClipModeClip : MTL::DepthClipModeClamp;
        state->depthBias = desc.raster.depthBias;
        state->listType = metal4::listType(desc.primitive);
        record.native = state.get();
        const Pipeline pipeline = impl.core.pool<Pipeline>().allocate(std::move(record));
        if (pipeline.bits == 0)
        {
            state->release();
            return std::unexpected(core::tableFull("pipeline", impl.core.pool<Pipeline>().capacity()));
        }
        state.release();
        if (hit)
        {
            // Counted once the pipeline exists, so a refused creation counts no hit.
            core::DeviceCounts::add(impl.core.counts().pipelineCacheHits);
        }
        return pipeline;
    }

    //==================================================================================================================
    Result<Pipeline> Device::createPipeline(const ComputePipelineDesc& desc)
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        core::PipelineRecord record = core::prepareComputePipeline(impl.core, desc);
        const metal4::AutoreleasePool pool;
        auto state = std::make_unique<metal4::PipelineState>();
        state->kind = core::PipelineKind::Compute;
        MTL4::LibraryFunctionDescriptor* function = newFunction(impl.core, desc.compute, core::ShaderStage::Compute, &state->computeThreads);
        MTL4::ComputePipelineDescriptor* descriptor = MTL4::ComputePipelineDescriptor::alloc()->init();
        descriptor->setComputeFunctionDescriptor(function);
        descriptor->setRequiredThreadsPerThreadgroup(state->computeThreads);
        descriptor->setLabel(metal4::makeString(record.label));
        NS::Error* error = nullptr;
        bool hit = false;
        state->compute = compileCompute(impl, descriptor, &error, hit);
        descriptor->release();
        releaseFunctions({ function });
        if (state->compute == nullptr)
        {
            return std::unexpected(
                Error{ ErrorCategory::DriverFailure, std::format("the pipeline \"{}\" was not created: {}", record.label, metal4::describe(error)) }
            );
        }
        record.native = state.get();
        const Pipeline pipeline = impl.core.pool<Pipeline>().allocate(std::move(record));
        if (pipeline.bits == 0)
        {
            state->release();
            return std::unexpected(core::tableFull("pipeline", impl.core.pool<Pipeline>().capacity()));
        }
        state.release();
        if (hit)
        {
            // Counted once the pipeline exists, so a refused creation counts no hit.
            core::DeviceCounts::add(impl.core.counts().pipelineCacheHits);
        }
        return pipeline;
    }

    //==================================================================================================================
    Result<std::vector<std::byte>> Device::pipelineCacheData()
    {
        DeviceImpl& impl = DeviceImpl::of(*this);
        return impl.pipelineCache.data(impl.device);
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
