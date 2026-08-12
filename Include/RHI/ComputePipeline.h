//----------------------------------------------------------------------------------------------------------------------
/// @file ComputePipeline.h
/// @brief Declares compute pipelines and their kernel and threadgroup descriptor.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include <cstdint>
#include <string_view>

namespace lmx::rhi {

class ShaderLibrary;

/// Describes the kernel and threadgroup shape of a compute pipeline.
///
/// threadsPerThreadgroup restates the kernel's own `[numthreads]` because the Slang-to-Metal path
/// does not carry it into the compiled library: the shading language expects the host to supply the
/// threadgroup size at dispatch. Stating it once here keeps CommandList::dispatch a count of
/// threadgroups rather than a second place where the shader's shape has to be repeated. It is the
/// caller's job to keep the two in step -- a mismatch is a shader that reads out of its own bounds,
/// not a pipeline-creation failure.
struct ComputePipelineDesc {
    ShaderLibrary* library = nullptr; ///< Shader library containing the kernel.
    std::string_view computeEntry;    ///< Compute-stage entry-point name.
    /// Threads per threadgroup in x, y, z; must equal the kernel's `[numthreads]` and each
    /// component must be at least one.
    uint32_t threadsPerThreadgroup[3] = {1, 1, 1};
    std::string_view label; ///< Diagnostic object label.
};
/// Represents an immutable compute pipeline.
class ComputePipeline {
public:
    /// Destroys the compute pipeline.
    virtual ~ComputePipeline() = default;
};

} // namespace lmx::rhi
