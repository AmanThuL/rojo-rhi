//----------------------------------------------------------------------------------------------------------------------
/// @file Indirect.h
/// @brief Declares the GPU-readable indirect dispatch and draw argument structures.
//----------------------------------------------------------------------------------------------------------------------
#pragma once
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace lmx::rhi {

/// Byte alignment every indirect-argument buffer offset must satisfy.
///
/// Four bytes, which is the alignment of the 32-bit members the three argument structs below are
/// made of and the alignment the backend's encoders require of the address they are handed. An
/// offset that is not a multiple of it is a caller error, not a slow path.
inline constexpr uint64_t kIndirectArgsAlignment = 4;

/// Layout contract shared by DispatchIndirectArgs, DrawIndirectArgs, and DrawIndexedIndirectArgs:
///
///   - each is standard-layout and made only of 4-byte scalars, so every member sits at its own
///     natural offset with no padding anywhere, including at the end -- an array of them is tightly
///     packed at sizeof(), and the static_asserts below pin that rather than trusting it;
///   - the buffer offset passed to the matching command is in bytes and must be a multiple of
///     kIndirectArgsAlignment;
///   - the GPU reads these fields directly out of the buffer, so a caller writing them from a
///     compute kernel must lay the same 32-bit words out in the same order. The kernel's own
///     structure declaration is not checked against this one by anything; the conformance tests are
///     what pin the two together.
///
/// The structs are RHI-owned rather than backend-shaped: this backend's encoders happen to consume
/// exactly these fields in exactly this order, and a backend whose native layout differs must
/// translate rather than silently reinterpret.
/// Arguments the GPU reads for an indirect dispatch.
struct DispatchIndirectArgs {
    uint32_t threadgroupsX = 0; ///< Threadgroups dispatched in x; threads come from the pipeline.
    uint32_t threadgroupsY = 0; ///< Threadgroups dispatched in y.
    uint32_t threadgroupsZ = 0; ///< Threadgroups dispatched in z.
};
static_assert(std::is_standard_layout_v<DispatchIndirectArgs>);
static_assert(sizeof(DispatchIndirectArgs) == 12);
static_assert(offsetof(DispatchIndirectArgs, threadgroupsX) == 0);
static_assert(offsetof(DispatchIndirectArgs, threadgroupsY) == 4);
static_assert(offsetof(DispatchIndirectArgs, threadgroupsZ) == 8);

/// Arguments the GPU reads for an indirect non-indexed draw.
struct DrawIndirectArgs {
    uint32_t vertexCount = 0; ///< Vertices drawn per instance.
    /// Instances drawn. Defaults to one because zero is a draw that silently does nothing.
    uint32_t instanceCount = 1;
    uint32_t firstVertex = 0;   ///< First vertex index, added to the shader's vertex ID.
    uint32_t firstInstance = 0; ///< First instance index, added to the shader's instance ID.
};
static_assert(std::is_standard_layout_v<DrawIndirectArgs>);
static_assert(sizeof(DrawIndirectArgs) == 16);
static_assert(offsetof(DrawIndirectArgs, vertexCount) == 0);
static_assert(offsetof(DrawIndirectArgs, instanceCount) == 4);
static_assert(offsetof(DrawIndirectArgs, firstVertex) == 8);
static_assert(offsetof(DrawIndirectArgs, firstInstance) == 12);

/// Arguments the GPU reads for an indirect indexed draw, matching drawIndexed's uint32 index model.
struct DrawIndexedIndirectArgs {
    uint32_t indexCount = 0; ///< Indices drawn per instance.
    /// Instances drawn. Defaults to one for the same reason DrawIndirectArgs::instanceCount does.
    uint32_t instanceCount = 1;
    uint32_t firstIndex = 0; ///< First index, as an element offset into the index buffer.
    /// Added to every index before the vertex is fetched. Signed, so a draw may address vertices
    /// before the ones its indices name.
    int32_t baseVertex = 0;
    uint32_t firstInstance = 0; ///< First instance index, added to the shader's instance ID.
};
static_assert(std::is_standard_layout_v<DrawIndexedIndirectArgs>);
static_assert(sizeof(DrawIndexedIndirectArgs) == 20);
static_assert(offsetof(DrawIndexedIndirectArgs, indexCount) == 0);
static_assert(offsetof(DrawIndexedIndirectArgs, instanceCount) == 4);
static_assert(offsetof(DrawIndexedIndirectArgs, firstIndex) == 8);
static_assert(offsetof(DrawIndexedIndirectArgs, baseVertex) == 12);
static_assert(offsetof(DrawIndexedIndirectArgs, firstInstance) == 16);

} // namespace lmx::rhi
