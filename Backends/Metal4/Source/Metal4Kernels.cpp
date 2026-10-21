//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Kernels.cpp
/// @brief Implements the Metal 4 backend's own kernels: the embedded library, their pipelines and a run on a list's encoder.
//----------------------------------------------------------------------------------------------------------------------

#include "Metal4Kernels.h"

#include <rojoRHI/RHI.h>

#include "Base/Assert.h"
#include "Core/CommandLists.h"
#include "Metal4Common.h"
#include "Metal4KernelLibrary.h"
#include "Metal4ListState.h"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <dispatch/dispatch.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace rojoRHI::metal4 {
    namespace {
        //==============================================================================================================
        /// Makes the compute pipeline of the function `name` of `library` with `compiler`, labelled `label`, specialised by
        /// `constants` when they are not null. Returns null with `error` set when Metal refuses it. The caller holds an
        /// autorelease pool.
        MTL::ComputePipelineState* newKernelPipeline(
            MTL4::Compiler* compiler,
            MTL::Library* library,
            std::string_view name,
            const MTL::FunctionConstantValues* constants,
            std::string_view label,
            NS::Error** error
        )
        {
            MTL4::LibraryFunctionDescriptor* function = MTL4::LibraryFunctionDescriptor::alloc()->init();
            function->setLibrary(library);
            function->setName(makeString(name));
            MTL4::SpecializedFunctionDescriptor* specialized = nullptr;
            if (constants != nullptr)
            {
                specialized = MTL4::SpecializedFunctionDescriptor::alloc()->init();
                specialized->setFunctionDescriptor(function);
                specialized->setConstantValues(constants);
            }
            MTL4::ComputePipelineDescriptor* descriptor = MTL4::ComputePipelineDescriptor::alloc()->init();
            descriptor->setComputeFunctionDescriptor(specialized != nullptr ? static_cast<MTL4::FunctionDescriptor*>(specialized) : function);
            descriptor->setLabel(makeString(label));
            *error = nullptr;
            MTL::ComputePipelineState* pipeline = compiler->newComputePipelineState(descriptor, nullptr, error);
            descriptor->release();
            if (specialized != nullptr)
            {
                specialized->release();
            }
            function->release();
            return pipeline;
        }

        /// The function and the label of one clear pipeline.
        struct ClearPipeline
        {
            std::string_view function; ///< The kernel function the pipeline specialises.
            std::string_view label;    ///< The pipeline's label.
        };

        /// The clear pipelines, indexed by `ClearKernel`. The 1D and 2D functions take the type as a function constant; the 3D
        /// function serves one type and takes none.
        constexpr std::array<ClearPipeline, kClearKernelCount> kClearPipelines{
            ClearPipeline{ "rojorhi_clear_1d", "rojorhi.kernel.clear.1D" },        ClearPipeline{ "rojorhi_clear_1d", "rojorhi.kernel.clear.1DArray" },
            ClearPipeline{ "rojorhi_clear_2d", "rojorhi.kernel.clear.2D" },        ClearPipeline{ "rojorhi_clear_2d", "rojorhi.kernel.clear.2DArray" },
            ClearPipeline{ "rojorhi_clear_3d", "rojorhi.kernel.clear.3D" },        ClearPipeline{ "rojorhi_clear_2d", "rojorhi.kernel.clear.Cube" },
            ClearPipeline{ "rojorhi_clear_2d", "rojorhi.kernel.clear.CubeArray" },
        };

        /// Where a counted draw's kernel pipeline goes, its function and its label.
        struct DrawPipeline
        {
            MTL::ComputePipelineState** pipeline; ///< The member of `Kernels` that keeps it.
            std::string_view function;            ///< The kernel function.
            std::string_view label;               ///< The pipeline's label.
        };
    } // namespace

    //==================================================================================================================
    Result<void> Kernels::create(MTL::Device* device, MTL4::Compiler* compiler)
    {
        // The default destructor makes the data copy the bytes, so the library owns what it was made from.
        dispatch_data_t data = dispatch_data_create(kKernelLibrary, sizeof(kKernelLibrary), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NS::Error* error = nullptr;
        library = device->newLibrary(data, &error);
        dispatch_release(data);
        if (library == nullptr)
        {
            return std::unexpected(
                Error{ ErrorCategory::DriverFailure, std::format("the kernel library rojorhi.kernels was not loaded: {}", describe(error)) }
            );
        }
        library->setLabel(makeString("rojorhi.kernels"));
        fill = newKernelPipeline(compiler, library, "rojorhi_fill", nullptr, "rojorhi.kernel.fill", &error);
        if (fill == nullptr)
        {
            return std::unexpected(
                Error{ ErrorCategory::DriverFailure, std::format("the kernel pipeline rojorhi.kernel.fill was not created: {}", describe(error)) }
            );
        }
        for (size_t i = 0; i < kClearKernelCount; ++i)
        {
            const ClearPipeline& made = kClearPipelines[i];
            MTL::FunctionConstantValues* constants = nullptr;
            if (static_cast<ClearKernel>(i) != ClearKernel::Tex3D)
            {
                // The function constant `kClearType` of Metal4Clear.metal selects the texture argument and the write.
                const auto type = static_cast<uint32_t>(i);
                constants = MTL::FunctionConstantValues::alloc()->init();
                constants->setConstantValue(&type, MTL::DataTypeUInt, NS::UInteger{ 0 });
            }
            clear[i] = newKernelPipeline(compiler, library, made.function, constants, made.label, &error);
            if (constants != nullptr)
            {
                constants->release();
            }
            if (clear[i] == nullptr)
            {
                return std::unexpected(
                    Error{ ErrorCategory::DriverFailure, std::format("the kernel pipeline {} was not created: {}", made.label, describe(error)) }
                );
            }
        }
        // The kernels of counted draws, which take every varying value as a parameter.
        const std::array<DrawPipeline, 6> drawPipelines{
            DrawPipeline{ &encodeDraw, "rojorhi_encode_draw", "rojorhi.kernel.encodeDraw" },
            DrawPipeline{ &encodeIndexed, "rojorhi_encode_indexed", "rojorhi.kernel.encodeIndexed" },
            DrawPipeline{ &writeDraw, "rojorhi_write_draw", "rojorhi.kernel.writeDraw" },
            DrawPipeline{ &writeIndexed, "rojorhi_write_indexed", "rojorhi.kernel.writeIndexed" },
            DrawPipeline{ &encodeMesh, "rojorhi_encode_mesh", "rojorhi.kernel.encodeMesh" },
            DrawPipeline{ &writeMesh, "rojorhi_write_mesh", "rojorhi.kernel.writeMesh" },
        };
        for (const DrawPipeline& made : drawPipelines)
        {
            *made.pipeline = newKernelPipeline(compiler, library, made.function, nullptr, made.label, &error);
            if (*made.pipeline == nullptr)
            {
                return std::unexpected(
                    Error{ ErrorCategory::DriverFailure, std::format("the kernel pipeline {} was not created: {}", made.label, describe(error)) }
                );
            }
        }
        return {};
    }

    //==================================================================================================================
    void Kernels::release()
    {
        for (MTL::ComputePipelineState** pipeline : std::array{ &encodeDraw, &encodeIndexed, &writeDraw, &writeIndexed, &encodeMesh, &writeMesh })
        {
            if (*pipeline != nullptr)
            {
                (*pipeline)->release();
                *pipeline = nullptr;
            }
        }
        for (MTL::ComputePipelineState*& pipeline : clear)
        {
            if (pipeline != nullptr)
            {
                pipeline->release();
                pipeline = nullptr;
            }
        }
        if (fill != nullptr)
        {
            fill->release();
            fill = nullptr;
        }
        if (library != nullptr)
        {
            library->release();
            library = nullptr;
        }
    }

    //==================================================================================================================
    uint32_t fillDispatchCount(uint64_t words)
    {
        return static_cast<uint32_t>((words + kFillDispatchWords - 1) / kFillDispatchWords);
    }

    //==================================================================================================================
    void bindKernel(core::CommandListCore& list, const ListState& state, Encoders& encoders, const MTL::ComputePipelineState* pipeline)
    {
        MTL4::ComputeCommandEncoder* encoder = encoders.computeEncoder();
        // A pipeline set on the open encoder stays set until it closes or another pipeline is set; setting it again is
        // redundant, which the validation layer reports.
        if (encoders.kernelPipeline() != pipeline)
        {
            encoder->setComputePipelineState(pipeline);
            core::noteNativeCall(list);
            encoders.setKernelPipeline(pipeline);
        }
        encoder->setArgumentTable(state.kernelTable);
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void bindKernelBatch(core::CommandListCore& list, const ListState& state, Encoders& encoders, const MTL::ComputePipelineState* pipeline)
    {
        MTL4::ComputeCommandEncoder* encoder = encoders.computeEncoder();
        // A batch encoder is opened kernels-only inside a render pass, where chapter 07 records no copy, fill, clear, compute
        // pipeline or dispatch (VU-rojo-cmd-0022, VU-rojo-cmd-0029 and the copy, fill and clear rules); so it holds a kernel only after its first
        // kernel bound the kernel table, which no later kernel unbinds.
        const bool tableBound = encoders.kernelPipeline() != nullptr;
        if (encoders.kernelPipeline() != pipeline)
        {
            encoder->setComputePipelineState(pipeline);
            core::noteNativeCall(list);
            encoders.setKernelPipeline(pipeline);
        }
        if (!tableBound)
        {
            encoder->setArgumentTable(state.kernelTable);
            core::noteNativeCall(list);
        }
    }

    //==================================================================================================================
    void dispatchKernelBatch(core::CommandListCore& list, const ListState& state, MTL4::ComputeCommandEncoder* encoder, MTL::Size groups, MTL::Size threads)
    {
        {
            // The validation layer autoreleases in each dispatch it checks, as for dispatchKernel.
            const CommandPool pool(state.commandPools);
            encoder->dispatchThreadgroups(groups, threads);
        }
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    void dispatchKernelRun(core::CommandListCore& list, Encoders& encoders, const KernelRun& run)
    {
        MTL4::ComputeCommandEncoder* encoder = encoders.computeEncoder();
        bindKernelBatch(list, *run.state, encoders, run.kernel);
        run.state->kernelTable->setAddress(run.params, 0);
        core::noteNativeCall(list);
        {
            // The validation layer autoreleases in each dispatch it checks, as for dispatchKernel. Apple GPUs take a grid that
            // is no multiple of the threadgroup, so the run's last threadgroup holds only its last draws.
            const CommandPool pool(run.state->commandPools);
            encoder->dispatchThreads(MTL::Size(run.count, 1, 1), MTL::Size(std::min(run.count, kDrawGroupThreads), 1, 1));
        }
        core::noteNativeCall(list);
    }

    //==================================================================================================================
    ClearKernel clearKernelOf(MTL::TextureType type)
    {
        switch (type)
        {
        case MTL::TextureType1D: return ClearKernel::Tex1D;
        case MTL::TextureType1DArray: return ClearKernel::Tex1DArray;
        case MTL::TextureType2DArray: return ClearKernel::Tex2DArray;
        case MTL::TextureType3D: return ClearKernel::Tex3D;
        case MTL::TextureTypeCube: return ClearKernel::Cube;
        case MTL::TextureTypeCubeArray: return ClearKernel::CubeArray;
        default: break;
        }
        // A storage texture is never multisample (VU-rojo-texture-0008), and the backend makes no texture buffer.
        ROJORHI_INTERNAL_ASSERT(type == MTL::TextureType2D, "a Storage texture has a texture type no clear kernel serves");
        return ClearKernel::Tex2D;
    }

    //==================================================================================================================
    void dispatchKernel(core::CommandListCore& list, const ListState& state, MTL4::ComputeCommandEncoder* encoder, MTL::Size groups, MTL::Size threads)
    {
        {
            // The Metal validation layer autoreleases an object in each dispatch it checks, so under it the dispatch holds a
            // pool, which leaves a thread without a pool of its own leaking nothing.
            const CommandPool pool(state.commandPools);
            encoder->dispatchThreadgroups(groups, threads);
        }
        core::noteNativeCall(list);
        // The caller's root and tables are bound again for its next dispatch. Its compute pipeline is set again at that
        // dispatch, which finds the kernel's set on the encoder.
        encoder->setArgumentTable(state.table);
        core::noteNativeCall(list);
    }
} // namespace rojoRHI::metal4
