//----------------------------------------------------------------------------------------------------------------------
/// @file NativeFrameBarriers.cpp
/// @brief Defines the D3D12 lowering of the composed frame's barrier batches and records them.
//----------------------------------------------------------------------------------------------------------------------
#include "NativeD3D12/NativeFrameBarriers.h"

namespace rojoRHI::bench {
    namespace {
        constexpr D3D12_BARRIER_SYNC kSyncCompute = D3D12_BARRIER_SYNC_COMPUTE_SHADING;
        constexpr D3D12_BARRIER_SYNC kSyncPixel = D3D12_BARRIER_SYNC_PIXEL_SHADING;
        constexpr D3D12_BARRIER_SYNC kSyncTarget = D3D12_BARRIER_SYNC_RENDER_TARGET;
        constexpr D3D12_BARRIER_SYNC kSyncDepth = D3D12_BARRIER_SYNC_DEPTH_STENCIL;
        constexpr D3D12_BARRIER_SYNC kSyncCopy = D3D12_BARRIER_SYNC_COPY;
        // The Copy stage on a Graphics list, a copy, the fill kernel or a clear through a target view, with no access: the
        // unordered-access clear's scope, which exists only for its access, is dropped.
        constexpr D3D12_BARRIER_SYNC kSyncCopyStage =
            D3D12_BARRIER_SYNC_COPY | D3D12_BARRIER_SYNC_COMPUTE_SHADING | D3D12_BARRIER_SYNC_RENDER_TARGET | D3D12_BARRIER_SYNC_DEPTH_STENCIL;

        constexpr D3D12_BARRIER_ACCESS kNoAccess = D3D12_BARRIER_ACCESS_NO_ACCESS;
        constexpr D3D12_BARRIER_ACCESS kTarget = D3D12_BARRIER_ACCESS_RENDER_TARGET;
        constexpr D3D12_BARRIER_ACCESS kDepthReadAccess = D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ;
        constexpr D3D12_BARRIER_ACCESS kDepthWriteAccess = D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
        constexpr D3D12_BARRIER_ACCESS kDepthReadWriteAccess = D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ | D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
        constexpr D3D12_BARRIER_ACCESS kShaderRead = D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
        constexpr D3D12_BARRIER_ACCESS kShaderWrite = D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
        constexpr D3D12_BARRIER_ACCESS kCopyRead = D3D12_BARRIER_ACCESS_COPY_SOURCE;

        constexpr D3D12_BARRIER_LAYOUT kUndefined = D3D12_BARRIER_LAYOUT_UNDEFINED;
        constexpr D3D12_BARRIER_LAYOUT kTargetLayout = D3D12_BARRIER_LAYOUT_RENDER_TARGET;
        constexpr D3D12_BARRIER_LAYOUT kDepthLayout = D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
        constexpr D3D12_BARRIER_LAYOUT kReadLayout = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
        constexpr D3D12_BARRIER_LAYOUT kWriteLayout = D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
        constexpr D3D12_BARRIER_LAYOUT kCopyLayout = D3D12_BARRIER_LAYOUT_COPY_SOURCE;

        //==============================================================================================================
        // A color target of a render pass into ShaderRead for the kernels: RenderTarget written -> compute reads.
        constexpr NativeTextureBarrier targetToKernels(FrameTexture texture)
        {
            return { texture, kSyncTarget, kSyncCompute, kTarget, kShaderRead, kTargetLayout, kReadLayout };
        }

        //==============================================================================================================
        // A color target of a render pass into ShaderRead for the next pass's fragment shader.
        constexpr NativeTextureBarrier targetToFragments(FrameTexture texture)
        {
            return { texture, kSyncTarget, kSyncPixel, kTarget, kShaderRead, kTargetLayout, kReadLayout };
        }

        //==============================================================================================================
        // A texture discarded into a color target behind work of `previous`, which named no access.
        constexpr NativeTextureBarrier discardToTarget(FrameTexture texture, D3D12_BARRIER_SYNC previous)
        {
            return { texture, previous, kSyncTarget, kNoAccess, kTarget, kUndefined, kTargetLayout };
        }

        //==============================================================================================================
        // A texture the fragment shader sampled, back into a color target that the pass loads and blends into.
        constexpr NativeTextureBarrier sampledToTarget(FrameTexture texture)
        {
            return { texture, kSyncPixel, kSyncTarget, kShaderRead, kTarget, kReadLayout, kTargetLayout };
        }

        constexpr NativeTextureBarrier kUnused{
            FrameTexture::Shadow, D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_NONE, kNoAccess, kNoAccess, kUndefined, kUndefined
        };
    } // namespace

    // Each batch of RojoFrame lowered by chapter 09's D3D12 rows, as RojoRHI's D3D12 backend lowers it: a stage's sync
    // keeps only the scopes its accesses run in; a side of a texture barrier with no access from LAYOUT_UNDEFINED takes
    // its own stage's sync, or the destination's when it names no stage; a side with no access out of a defined layout
    // names the accesses that layout admits; ShaderRead lowers to the generic LAYOUT_SHADER_RESOURCE, which the direct and
    // compute queues share, and a texture read in ShaderWrite is read through its unordered-access view.
    const std::array<NativeBatch, kFrameBatchCount> kFrameBarriers{ {
        // albedo, normal, material: Undefined -> ColorAttachment, ComputeShader/None -> ColorAttachment/ColorAttachmentWrite;
        // depth: Undefined -> DepthStencilWrite, DepthStencil/None -> DepthStencil/DepthStencilRead+DepthStencilWrite.
        { FrameBatch::Geometry,
          4,
          { { discardToTarget(FrameTexture::Albedo, kSyncCompute),
              discardToTarget(FrameTexture::Normal, kSyncCompute),
              discardToTarget(FrameTexture::Material, kSyncCompute),
              { FrameTexture::Depth, kSyncDepth, kSyncDepth, kNoAccess, kDepthReadWriteAccess, kUndefined, kDepthLayout } } } },
        // albedo, normal, material: ColorAttachment -> ShaderRead, ColorAttachment/ColorAttachmentWrite ->
        // ComputeShader/ShaderRead; depth: DepthStencilWrite -> ShaderRead, DepthStencil/DepthStencilWrite ->
        // ComputeShader/ShaderRead. The ao list on the compute queue reads them behind the wait on gbufferDone.
        { FrameBatch::GbufferClose,
          4,
          { { targetToKernels(FrameTexture::Albedo),
              targetToKernels(FrameTexture::Normal),
              targetToKernels(FrameTexture::Material),
              { FrameTexture::Depth, kSyncDepth, kSyncCompute, kDepthWriteAccess, kShaderRead, kDepthLayout, kReadLayout } } } },
        // aoRaw, ao: Undefined -> ShaderWrite, None/None -> ComputeShader/ShaderWrite. No source stage: the destination's
        // sync before, so the layout changes and discards behind the kernels of the previous frame.
        { FrameBatch::AoStart,
          2,
          { { { FrameTexture::AoRaw, kSyncCompute, kSyncCompute, kNoAccess, kShaderWrite, kUndefined, kWriteLayout },
              { FrameTexture::Ao, kSyncCompute, kSyncCompute, kNoAccess, kShaderWrite, kUndefined, kWriteLayout },
              kUnused,
              kUnused } } },
        // aoRaw: ShaderWrite -> ShaderRead, ComputeShader/ShaderWrite -> ComputeShader/ShaderRead.
        { FrameBatch::AoRaw,
          1,
          { { { FrameTexture::AoRaw, kSyncCompute, kSyncCompute, kShaderWrite, kShaderRead, kWriteLayout, kReadLayout }, kUnused, kUnused, kUnused } } },
        // ao: ShaderWrite -> ShaderRead, ComputeShader/ShaderWrite -> ComputeShader/ShaderRead. The lighting list reads ao
        // behind the wait on aoDone.
        { FrameBatch::AoClose,
          1,
          { { { FrameTexture::Ao, kSyncCompute, kSyncCompute, kShaderWrite, kShaderRead, kWriteLayout, kReadLayout }, kUnused, kUnused, kUnused } } },
        // shadow, every layer: Undefined -> DepthStencilWrite, ComputeShader/None -> DepthStencil/DepthStencilRead+DepthStencilWrite.
        { FrameBatch::Shadows,
          1,
          { { { FrameTexture::Shadow, kSyncCompute, kSyncDepth, kNoAccess, kDepthReadWriteAccess, kUndefined, kDepthLayout }, kUnused, kUnused, kUnused } } },
        // shadow: DepthStencilWrite -> ShaderRead, DepthStencil/DepthStencilWrite -> ComputeShader/ShaderRead; hdr:
        // Undefined -> ShaderWrite, FragmentShader/None -> ComputeShader/ShaderWrite.
        { FrameBatch::Lighting,
          2,
          { { { FrameTexture::Shadow, kSyncDepth, kSyncCompute, kDepthWriteAccess, kShaderRead, kDepthLayout, kReadLayout },
              { FrameTexture::Hdr, kSyncPixel, kSyncCompute, kNoAccess, kShaderWrite, kUndefined, kWriteLayout },
              kUnused,
              kUnused } } },
        // hdr: ShaderWrite -> ColorAttachment, ComputeShader/ShaderWrite -> ColorAttachment/ColorAttachmentRead+Write;
        // depth: ShaderRead -> DepthStencilWrite, ComputeShader/None -> DepthStencil/DepthStencilRead: a source with no
        // access out of LAYOUT_SHADER_RESOURCE names the shader reads that layout admits.
        { FrameBatch::Forward,
          2,
          { { { FrameTexture::Hdr, kSyncCompute, kSyncTarget, kShaderWrite, kTarget, kWriteLayout, kTargetLayout },
              { FrameTexture::Depth, kSyncCompute, kSyncDepth, kShaderRead, kDepthReadAccess, kReadLayout, kDepthLayout },
              kUnused,
              kUnused } } },
        // hdr: ColorAttachment -> ShaderRead, ColorAttachment/ColorAttachmentWrite -> FragmentShader/ShaderRead; bloom0:
        // Undefined -> ColorAttachment, FragmentShader/None -> ColorAttachment/ColorAttachmentWrite.
        { FrameBatch::BloomDown0, 2, { { targetToFragments(FrameTexture::Hdr), discardToTarget(FrameTexture::Bloom0, kSyncPixel), kUnused, kUnused } } },
        // bloom0 -> ShaderRead, bloom1 -> ColorAttachment, as before pass 9.
        { FrameBatch::BloomDown1, 2, { { targetToFragments(FrameTexture::Bloom0), discardToTarget(FrameTexture::Bloom1, kSyncPixel), kUnused, kUnused } } },
        // bloom1: ColorAttachment -> ShaderRead; bloom0: ShaderRead -> ColorAttachment, FragmentShader/None ->
        // ColorAttachment/ColorAttachmentRead+Write.
        { FrameBatch::BloomUp1, 2, { { targetToFragments(FrameTexture::Bloom1), sampledToTarget(FrameTexture::Bloom0), kUnused, kUnused } } },
        // bloom0 -> ShaderRead, hdr -> ColorAttachment, as before pass 11.
        { FrameBatch::BloomUp0, 2, { { targetToFragments(FrameTexture::Bloom0), sampledToTarget(FrameTexture::Hdr), kUnused, kUnused } } },
        // hdr: ColorAttachment -> ShaderRead; ldr: Undefined -> ColorAttachment, Copy/None -> ColorAttachment/
        // ColorAttachmentWrite, behind the previous frame's copy of ldr: the Copy stage of a Graphics list.
        { FrameBatch::ToneMap, 2, { { targetToFragments(FrameTexture::Hdr), discardToTarget(FrameTexture::Ldr, kSyncCopyStage), kUnused, kUnused } } },
        // ldr: ColorAttachment -> CopySrc, ColorAttachment/ColorAttachmentWrite -> Copy/CopyRead: the Copy stage keeps
        // the copy scope, the one its access runs in.
        { FrameBatch::Readback,
          1,
          { { { FrameTexture::Ldr, kSyncTarget, kSyncCopy, kTarget, kCopyRead, kTargetLayout, kCopyLayout }, kUnused, kUnused, kUnused } } },
    } };

    //==================================================================================================================
    const NativeBatch& nativeBatch(FrameBatch batch)
    {
        return kFrameBarriers[static_cast<size_t>(batch)];
    }

    //==================================================================================================================
    D3D12_BARRIER_LAYOUT frameEndLayout(FrameTexture texture)
    {
        D3D12_BARRIER_LAYOUT layout = D3D12_BARRIER_LAYOUT_COMMON;
        for (const NativeBatch& batch : kFrameBarriers)
        {
            for (uint32_t index = 0; index < batch.count; ++index)
            {
                if (batch.textures[index].texture == texture)
                {
                    layout = batch.textures[index].layoutAfter;
                }
            }
        }
        return layout;
    }

    //==================================================================================================================
    D3D12_TEXTURE_BARRIER textureBarrier(const NativeTextureBarrier& row, ID3D12Resource* resource, uint32_t layers)
    {
        D3D12_TEXTURE_BARRIER barrier{};
        barrier.SyncBefore = row.syncBefore;
        barrier.SyncAfter = row.syncAfter;
        barrier.AccessBefore = row.accessBefore;
        barrier.AccessAfter = row.accessAfter;
        barrier.LayoutBefore = row.layoutBefore;
        barrier.LayoutAfter = row.layoutAfter;
        barrier.pResource = resource;
        barrier.Subresources.IndexOrFirstMipLevel = 0;
        barrier.Subresources.NumMipLevels = 1;
        barrier.Subresources.FirstArraySlice = 0;
        barrier.Subresources.NumArraySlices = layers;
        barrier.Subresources.FirstPlane = 0;
        barrier.Subresources.NumPlanes = 1;
        barrier.Flags = row.layoutBefore == D3D12_BARRIER_LAYOUT_UNDEFINED ? D3D12_TEXTURE_BARRIER_FLAG_DISCARD : D3D12_TEXTURE_BARRIER_FLAG_NONE;
        return barrier;
    }

    //==================================================================================================================
    void emitBarriers(ID3D12GraphicsCommandList9& list, std::span<const D3D12_TEXTURE_BARRIER> barriers, NativeCalls& calls)
    {
        D3D12_BARRIER_GROUP group{};
        group.Type = D3D12_BARRIER_TYPE_TEXTURE;
        group.NumBarriers = static_cast<UINT32>(barriers.size());
        group.pTextureBarriers = barriers.data();
        list.Barrier(1, &group);
        ++calls.record;
        ++calls.barriers;
    }
} // namespace rojoRHI::bench
