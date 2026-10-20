//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Kernels.h
/// @brief Declares the Metal 4 backend's own compute kernels: their library and pipelines, and how a list runs one.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Metal4Encoders.h"
#include "Metal4ListState.h"

#include <Metal/Metal.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace rojoRHI::metal4 {
    /// Threads per threadgroup of the fill kernel, one 32-bit word each.
    constexpr uint32_t kFillGroupThreads = 64;

    /// The most words one dispatch of the fill kernel writes; a longer fill takes several.
    constexpr uint64_t kFillDispatchWords = uint64_t{ 1 } << 31;

    /// The parameters of one fill dispatch, laid out as the fill kernel reads them from `buffer(0)`.
    struct FillParams
    {
        uint32_t pattern; ///< The word written, its bytes in little-endian order.
        uint32_t words;   ///< How many words the dispatch writes from `buffer(1)`.
    };
    // The fill kernel declares the same struct in Metal4Fill.metal; the two must keep this size and member order.
    static_assert(sizeof(FillParams) == 8);

    /// The Metal texture types a `Storage` texture can have, each with its own clear pipeline. Multisample textures have no
    /// storage usage (VU-rojo-texture-0008).
    enum class ClearKernel : uint8_t
    {
        Tex1D,      ///< `MTL::TextureType1D`.
        Tex1DArray, ///< `MTL::TextureType1DArray`.
        Tex2D,      ///< `MTL::TextureType2D`.
        Tex2DArray, ///< `MTL::TextureType2DArray`.
        Tex3D,      ///< `MTL::TextureType3D`.
        Cube,       ///< `MTL::TextureTypeCube`.
        CubeArray,  ///< `MTL::TextureTypeCubeArray`.
    };

    /// How many clear pipelines there are, one per `ClearKernel`.
    constexpr size_t kClearKernelCount = 7;

    /// The edge of a clear kernel's threadgroup, which is `kClearGroupEdge` by `kClearGroupEdge` by 1 threads.
    constexpr uint32_t kClearGroupEdge = 8;

    /// The parameters of one clear dispatch, which covers one mip of the range, laid out as the clear kernels read them from
    /// `buffer(0)`.
    struct ClearParams
    {
        float color[4];       ///< The value written, red to alpha; a storage format converts it as a shader write does.
        uint32_t baseLayer;   ///< The first layer cleared, counting faces for cube types; 0 for a 3D texture.
        uint32_t mip;         ///< The mip cleared.
        uint32_t width;       ///< The mip's width in texels.
        uint32_t height;      ///< The mip's height in texels; 1 for 1D types.
        uint32_t layers;      ///< The layers cleared, or the mip's depth for a 3D texture.
        uint32_t reserved[3]; ///< Pads the struct to the size of its Metal counterpart, whose `float4` aligns it to 16 bytes.
    };
    // The clear kernels declare the same struct in Metal4Clear.metal; the two must keep this size and member order.
    static_assert(sizeof(ClearParams) == 48);

    /// Threads per threadgroup of the kernels that encode a counted draw, one record each.
    constexpr uint32_t kDrawGroupThreads = 64;

    /// The parameters of one counted draw's kernel, laid out as the kernels of Metal4DrawIndirect.metal read them from
    /// `buffer(0)`. Every address is a GPU address.
    struct DrawParams
    {
        /// The encode kernels: the address of the indirect command buffer's `MTL::ResourceID`, which the kernel reads as a
        /// `device` struct holding a `command_buffer`; 0 for the writer kernels.
        uint64_t icb;
        uint64_t records;     ///< The first `DrawIndirectArgs` or `DrawIndexedIndirectArgs` record.
        uint64_t count;       ///< The draw count, or 0 for none, which means `maxCount`.
        uint64_t output;      ///< The execution range the encode kernels write, or the native record the writer kernels write.
        uint64_t indices;     ///< The index buffer of an indexed draw; 0 otherwise.
        uint32_t maxCount;    ///< The most records drawn.
        uint32_t primitive;   ///< The `MTL::PrimitiveType` value of the draws.
        uint32_t indexType;   ///< The `MTL::IndexType` value of an indexed draw.
        uint32_t reserved[3]; ///< Pads the struct to 64 bytes, where the indirect command buffer's resource ID follows.
    };
    // The draw kernels declare the same struct in Metal4DrawIndirect.metal; the two must keep this size and these offsets.
    static_assert(sizeof(DrawParams) == 64);
    static_assert(offsetof(DrawParams, records) == 8);
    static_assert(offsetof(DrawParams, count) == 16);
    static_assert(offsetof(DrawParams, output) == 24);
    static_assert(offsetof(DrawParams, indices) == 32);
    static_assert(offsetof(DrawParams, maxCount) == 40);
    static_assert(offsetof(DrawParams, primitive) == 44);
    static_assert(offsetof(DrawParams, indexType) == 48);
    // The writer kernels write the native records Metal's indirect draws read.
    static_assert(sizeof(MTL::DrawPrimitivesIndirectArguments) == 16);
    static_assert(sizeof(MTL::DrawIndexedPrimitivesIndirectArguments) == 20);
    static_assert(sizeof(MTL::DispatchThreadgroupsIndirectArguments) == 12);

    /// The parameters of one counted mesh draw's kernel, laid out as the mesh kernels of Metal4DrawIndirect.metal read them
    /// from `buffer(0)`. Every address is a GPU address. A mesh command binds its buffers itself, since nothing reaches it from
    /// the render encoder's table, so the parameters carry each address the four reserved bindings hold (ADR 0018).
    struct MeshParams
    {
        /// The encode kernel: the address of the indirect command buffer's `MTL::ResourceID`; 0 for the writer kernel.
        uint64_t icb;
        uint64_t records;          ///< The first `DrawMeshTasksIndirectArgs` record.
        uint64_t count;            ///< The draw count, or 0 for none, which means `maxCount`.
        uint64_t output;           ///< The execution range the encode kernel writes, or the native record the writer writes.
        uint64_t root;             ///< `buffer(0)`: the root recorded when the draw was recorded; 0 for none.
        uint64_t views;            ///< `buffer(1)`: the device's view table.
        uint64_t samplers;         ///< `buffer(2)`: the device's sampler table.
        uint32_t maxCount;         ///< The most records drawn.
        uint32_t objectThreads[3]; ///< The bound pipeline's task threadgroup size; zero without a task entry.
        uint32_t meshThreads[3];   ///< The bound pipeline's mesh threadgroup size.
        uint32_t reserved[3];      ///< Pads the struct to 96 bytes, where the indirect command buffer's ID or the native record follows.
    };
    // The mesh kernels declare the same struct in Metal4DrawIndirect.metal; the two must keep this size and these offsets.
    static_assert(sizeof(MeshParams) == 96);
    static_assert(offsetof(MeshParams, root) == 32);
    static_assert(offsetof(MeshParams, views) == 40);
    static_assert(offsetof(MeshParams, samplers) == 48);
    static_assert(offsetof(MeshParams, maxCount) == 56);
    static_assert(offsetof(MeshParams, objectThreads) == 60);
    static_assert(offsetof(MeshParams, meshThreads) == 72);

    /// Where, in the frame-arena reservation of a counted mesh draw that starts with its `MeshParams`, the indirect command
    /// buffer's resource ID or the writer kernel's native record lies.
    constexpr uint64_t kMeshOutputOffset = sizeof(MeshParams);

    /// Where, in the same reservation, the execution range of a mesh indirect command buffer lies.
    constexpr uint64_t kMeshRangeOffset = kMeshOutputOffset + 16;

    /// Returns the clear pipeline that serves a texture of `type`, which a `Storage` texture can have.
    ClearKernel clearKernelOf(MTL::TextureType type);

    /// The backend's own kernels: the library the build embeds, loaded once per device, and a pipeline per kernel made from it
    /// when the device is created. Each pointer is one retained reference. Neither the objects nor the calls that make them are
    /// counted: they are the device's own.
    struct Kernels
    {
        MTL::Library* library = nullptr;           ///< The embedded kernel library, labelled `rojorhi.kernels`.
        MTL::ComputePipelineState* fill = nullptr; ///< Writes a 32-bit pattern over a range of words.
        /// Writes a `float4` over one mip of a range of a `Storage` texture, one pipeline per `ClearKernel`.
        std::array<MTL::ComputePipelineState*, kClearKernelCount> clear = {};
        /// Encodes the records of a counted draw below its count into an indirect command buffer and writes its range.
        MTL::ComputePipelineState* encodeDraw = nullptr;
        /// Encodes the records of a counted indexed draw below its count into an indirect command buffer and writes its range.
        MTL::ComputePipelineState* encodeIndexed = nullptr;
        /// Writes the one record of a counted draw of `maxCount` 1 as a native record.
        MTL::ComputePipelineState* writeDraw = nullptr;
        /// Writes the one record of a counted indexed draw of `maxCount` 1 as a native record.
        MTL::ComputePipelineState* writeIndexed = nullptr;
        /// Encodes the mesh records of a counted mesh draw below its count into a mesh indirect command buffer, each command
        /// binding the reserved buffers on the object, mesh and fragment stages, and writes its range.
        MTL::ComputePipelineState* encodeMesh = nullptr;
        /// Writes the one record of a counted mesh draw of `maxCount` 1 as a native record.
        MTL::ComputePipelineState* writeMesh = nullptr;

        /// Loads the embedded library on `device` and makes every kernel pipeline with `compiler`. The caller holds an
        /// autorelease pool. On failure it returns `DriverFailure` naming the library or the kernel Metal refused, and the caller
        /// releases what exists.
        Result<void> create(MTL::Device* device, MTL4::Compiler* compiler);

        /// Releases every object that exists, pipelines before the library, and clears each pointer. The caller holds an
        /// autorelease pool and no queue holds work that uses them.
        void release();
    };

    /// Returns how many dispatches a fill of `words` words takes: one per `kFillDispatchWords` words or part of it.
    uint32_t fillDispatchCount(uint64_t words);

    /// Sets `pipeline` on the open compute encoder of `encoders`, unless it is already set there, and the list's kernel table,
    /// counting each call on `list`. The caller then binds the kernel's arguments in `state.kernelTable`, counting each, and
    /// calls `dispatchKernel`.
    void bindKernel(core::CommandListCore& list, const ListState& state, Encoders& encoders, const MTL::ComputePipelineState* pipeline);

    /// Dispatches `groups` threadgroups of `threads` threads of the kernel `bindKernel` set, then sets the list's own table
    /// again, counting both calls on `list`. The encoder takes the kernel table's bindings as they stand at the dispatch, so the
    /// next kernel may rebind them.
    void dispatchKernel(core::CommandListCore& list, const ListState& state, MTL4::ComputeCommandEncoder* encoder, MTL::Size groups, MTL::Size threads);

    /// Sets `pipeline` on the open compute encoder of `encoders`, unless it is already set there, and the list's kernel table
    /// when the encoder holds no kernel yet, counting each call on `list`: the batch form, for an encoder that runs only
    /// kernels, as the one a render pass opens ahead of its render encoder for its counted draws. Such an encoder opens
    /// kernels-only (`Encoders::compute`) and never takes the list's table, so the kernel table stays bound from its first
    /// kernel to its end. The caller then binds the kernel's arguments in `state.kernelTable`, counting each, and calls
    /// `dispatchKernelBatch`.
    void bindKernelBatch(core::CommandListCore& list, const ListState& state, Encoders& encoders, const MTL::ComputePipelineState* pipeline);

    /// Dispatches `groups` threadgroups of `threads` threads of the kernel `bindKernelBatch` set, counting the call on `list`;
    /// the kernel table stays bound for the next kernel of the batch.
    void dispatchKernelBatch(core::CommandListCore& list, const ListState& state, MTL4::ComputeCommandEncoder* encoder, MTL::Size groups, MTL::Size threads);
} // namespace rojoRHI::metal4
