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
    constexpr uint32_t kFillGroupThreads = 64;

    // A longer fill takes several dispatches.
    constexpr uint64_t kFillDispatchWords = uint64_t{ 1 } << 31;

    // Matches the layout the fill kernel reads from `buffer(0)`.
    struct FillParams
    {
        uint32_t pattern; // Its bytes in little-endian order.
        uint32_t words;
    };
    // The fill kernel declares the same struct in Metal4Fill.metal; the two must keep this size and member order.
    static_assert(sizeof(FillParams) == 8);

    // Multisample textures have no storage usage (VU-rojo-texture-0008).
    enum class ClearKernel : uint8_t
    {
        Tex1D,
        Tex1DArray,
        Tex2D,
        Tex2DArray,
        Tex3D,
        Cube,
        CubeArray,
    };

    constexpr size_t kClearKernelCount = 7;

    // Threadgroups are `kClearGroupEdge` by `kClearGroupEdge` by 1 threads.
    constexpr uint32_t kClearGroupEdge = 8;

    // One dispatch covers one mip of the range.
    struct ClearParams
    {
        float color[4];     // Red to alpha; a storage format converts it as a shader write does.
        uint32_t baseLayer; // Counting faces for cube types; 0 for a 3D texture.
        uint32_t mip;
        uint32_t width;
        uint32_t height;      // 1 for 1D types.
        uint32_t layers;      // Or the mip's depth for a 3D texture.
        uint32_t reserved[3]; // Pads to the size of the Metal counterpart, whose `float4` aligns it to 16 bytes.
    };
    // The clear kernels declare the same struct in Metal4Clear.metal; the two must keep this size and member order.
    static_assert(sizeof(ClearParams) == 48);

    constexpr uint32_t kDrawGroupThreads = 64;

    // Every address is a GPU address. A command binds its own record, so it binds its buffers itself, and the
    // parameters carry each address the four reserved bindings hold (ADR 0018, ADR 0028).
    struct DrawParams
    {
        // The address of the indirect command buffer's `MTL::ResourceID`, read as a `device` struct holding a
        // `command_buffer`; 0 for the writer kernels.
        uint64_t icb;
        uint64_t records; // The first `DrawIndirectArgs` or `DrawIndexedIndirectArgs` record.
        uint64_t count;   // 0 means `maxCount`.
        uint64_t output;  // The execution range the encode kernels write, or the native record the writer kernels write.
        uint64_t indices; // 0 unless indexed.
        uint64_t root;    // The root recorded with the draw; 0 for none.
        uint64_t views;
        uint64_t samplers;
        uint32_t maxCount;
        uint32_t primitive;   // An `MTL::PrimitiveType` value.
        uint32_t indexType;   // An `MTL::IndexType` value.
        uint32_t reserved[5]; // Pads the struct to 96 bytes, where the indirect command buffer's resource ID follows.
    };
    // The draw kernels declare the same struct in Metal4DrawIndirect.metal; the two must keep this size and these offsets.
    static_assert(sizeof(DrawParams) == 96);
    static_assert(offsetof(DrawParams, records) == 8);
    static_assert(offsetof(DrawParams, count) == 16);
    static_assert(offsetof(DrawParams, output) == 24);
    static_assert(offsetof(DrawParams, indices) == 32);
    static_assert(offsetof(DrawParams, root) == 40);
    static_assert(offsetof(DrawParams, views) == 48);
    static_assert(offsetof(DrawParams, samplers) == 56);
    static_assert(offsetof(DrawParams, maxCount) == 64);
    static_assert(offsetof(DrawParams, primitive) == 68);
    static_assert(offsetof(DrawParams, indexType) == 72);
    // The writer kernels write the native records Metal's indirect draws read.
    static_assert(sizeof(MTL::DrawPrimitivesIndirectArguments) == 16);
    static_assert(sizeof(MTL::DrawIndexedPrimitivesIndirectArguments) == 20);
    static_assert(sizeof(MTL::DispatchThreadgroupsIndirectArguments) == 12);

    // Every address is a GPU address. Nothing reaches a mesh command from the render encoder's table, so it binds its
    // buffers itself, and the parameters carry each address the four reserved bindings hold (ADR 0018).
    struct MeshParams
    {
        // The address of the indirect command buffer's `MTL::ResourceID`; 0 for the writer kernel.
        uint64_t icb;
        uint64_t records; // The first `DrawMeshTasksIndirectArgs` record.
        uint64_t count;   // 0 means `maxCount`.
        uint64_t output;  // The execution range the encode kernel writes, or the native record the writer writes.
        uint64_t root;    // The root recorded with the draw; 0 for none.
        uint64_t views;
        uint64_t samplers;
        uint32_t maxCount;
        uint32_t objectThreads[3]; // Zero without a task entry.
        uint32_t meshThreads[3];
        uint32_t reserved[3]; // Pads the struct to 96 bytes, where the indirect command buffer's ID or the native record follows.
    };
    // The mesh kernels declare the same struct in Metal4DrawIndirect.metal; the two must keep this size and these offsets.
    static_assert(sizeof(MeshParams) == 96);
    static_assert(offsetof(MeshParams, root) == 32);
    static_assert(offsetof(MeshParams, views) == 40);
    static_assert(offsetof(MeshParams, samplers) == 48);
    static_assert(offsetof(MeshParams, maxCount) == 56);
    static_assert(offsetof(MeshParams, objectThreads) == 60);
    static_assert(offsetof(MeshParams, meshThreads) == 72);

    // `DrawParams`, then either the indirect command buffer's resource ID and the execution range, or the native
    // record. A run of writer kernels reads its draws' parameters this far apart (`kDrawStride` in
    // Metal4DrawIndirect.metal).
    constexpr uint64_t kDrawReservation = 128;

    // Laid out as `kDrawReservation` is, with `MeshParams` first (`kMeshStride` in Metal4DrawIndirect.metal).
    constexpr uint64_t kMeshReservation = 128;
    static_assert(kDrawReservation == 128 && kMeshReservation == 128, "must match kDrawStride/kMeshStride in Metal4DrawIndirect.metal");

    // The indirect command buffer's resource ID or the writer kernel's native record.
    constexpr uint64_t kMeshOutputOffset = sizeof(MeshParams);

    constexpr uint64_t kMeshRangeOffset = kMeshOutputOffset + 16;

    ClearKernel clearKernelOf(MTL::TextureType type);

    // Loaded once per device, with a pipeline per kernel made at device creation. Each pointer is one retained
    // reference. Neither the objects nor the calls that make them are counted: they are the device's own.
    struct Kernels
    {
        MTL::Library* library = nullptr; // Labelled `rojorhi.kernels`.
        MTL::ComputePipelineState* fill = nullptr;
        // One pipeline per `ClearKernel`.
        std::array<MTL::ComputePipelineState*, kClearKernelCount> clear = {};
        // Each command binds the reserved buffers on the vertex and fragment stages.
        MTL::ComputePipelineState* encodeDraw = nullptr;
        MTL::ComputePipelineState* encodeIndexed = nullptr;
        MTL::ComputePipelineState* writeDraw = nullptr;
        MTL::ComputePipelineState* writeIndexed = nullptr;
        // Each command binds the reserved buffers on the object, mesh and fragment stages.
        MTL::ComputePipelineState* encodeMesh = nullptr;
        MTL::ComputePipelineState* writeMesh = nullptr;

        // The caller holds an autorelease pool and releases what exists on failure.
        Result<void> create(MTL::Device* device, MTL4::Compiler* compiler);

        // Pipelines before the library. The caller holds an autorelease pool and no queue holds work that uses them.
        void release();
    };

    uint32_t fillDispatchCount(uint64_t words);

    // The caller then binds the kernel's arguments in `state.kernelTable` and calls `dispatchKernel`.
    void bindKernel(core::CommandListCore& list, const ListState& state, Encoders& encoders, const MTL::ComputePipelineState* pipeline);

    // Then sets the list's own table again. The encoder takes the kernel table's bindings as they stand at the
    // dispatch, so the next kernel may rebind them.
    void dispatchKernel(core::CommandListCore& list, const ListState& state, MTL4::ComputeCommandEncoder* encoder, MTL::Size groups, MTL::Size threads);

    // For an encoder that runs only kernels, such as the one a render pass opens ahead of its render encoder for its
    // counted draws. It never takes the list's table, so the kernel table stays bound from its first kernel to its end.
    void bindKernelBatch(core::CommandListCore& list, const ListState& state, Encoders& encoders, const MTL::ComputePipelineState* pipeline);

    // The kernel table stays bound for the next kernel of the batch.
    void dispatchKernelBatch(core::CommandListCore& list, const ListState& state, MTL4::ComputeCommandEncoder* encoder, MTL::Size groups, MTL::Size threads);

    // Thread `i` reads the parameters at `run.params` plus `i` times `run.stride`.
    void dispatchKernelRun(core::CommandListCore& list, Encoders& encoders, const KernelRun& run);
} // namespace rojoRHI::metal4
