//----------------------------------------------------------------------------------------------------------------------
/// @file FrameLayout.h
/// @brief Declares every resource, record layout, pass, dispatch size and output of the composed frame, shared by both arms.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/Scene.h"

#include <rojoRHI/RHI.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace rojoRHI::bench {
    // Frame slots: records[s] and readback[s] exist once per slot, and frame f uses slot f mod kFrameSlots.
    inline constexpr uint32_t kFrameSlots = 3;

    /// The slot frame `frameIndex` (counted from 0, warm-up included) records into.
    constexpr uint32_t slotOf(uint32_t frameIndex)
    {
        return frameIndex % kFrameSlots;
    }

    // The textures, in creation order; indexes kTextures.
    enum class FrameTexture : uint8_t
    {
        Shadow,
        Albedo,
        Normal,
        Material,
        Depth,
        AoRaw,
        Ao,
        Hdr,
        Ldr,
        Bloom0,
        Bloom1,
    };

    inline constexpr uint32_t kTextureCount = 11;

    // One texture of the frame. A view of it is created per use by the arm, with the type the shaders read: Tex2D for
    // every texture, Tex2DArray over all three layers of `shadow`. The sampled views of `depth` and `shadow` use
    // TextureAspect::Depth (VU-rojo-texture-0027); every other view is Color. A view's usage is Sampled except the
    // Storage views of aoRaw, ao and hdr that the kernels write.
    struct TextureSpec
    {
        std::string_view label; ///< The creation label, also the key of its dump (`shadow` dumps one file per layer).
        TextureType type;
        Format format;
        Extent3D extent;
        uint32_t layers;
        TextureUsage usage;
        uint32_t bytesPerTexel;
    };

    inline constexpr TextureUsage kSampledColor = TextureUsage::Sampled | TextureUsage::RenderTarget;

    inline constexpr std::array<TextureSpec, kTextureCount> kTextures{ {
        { "frame.shadow", TextureType::Tex2DArray, Format::D32Float, { 2048, 2048, 1 }, 3, TextureUsage::Sampled | TextureUsage::DepthStencil, 4 },
        { "frame.albedo", TextureType::Tex2D, Format::RGBA8Unorm, { kFrameWidth, kFrameHeight, 1 }, 1, kSampledColor, 4 },
        { "frame.normal", TextureType::Tex2D, Format::RGBA16Float, { kFrameWidth, kFrameHeight, 1 }, 1, kSampledColor, 8 },
        { "frame.material", TextureType::Tex2D, Format::RGBA8Unorm, { kFrameWidth, kFrameHeight, 1 }, 1, kSampledColor, 4 },
        { "frame.depth", TextureType::Tex2D, Format::D32Float, { kFrameWidth, kFrameHeight, 1 }, 1, TextureUsage::Sampled | TextureUsage::DepthStencil, 4 },
        { "frame.aoRaw", TextureType::Tex2D, Format::R8Unorm, { kFrameWidth, kFrameHeight, 1 }, 1, TextureUsage::Sampled | TextureUsage::Storage, 1 },
        { "frame.ao", TextureType::Tex2D, Format::R8Unorm, { kFrameWidth, kFrameHeight, 1 }, 1, TextureUsage::Sampled | TextureUsage::Storage, 1 },
        { "frame.hdr",
          TextureType::Tex2D,
          Format::RGBA16Float,
          { kFrameWidth, kFrameHeight, 1 },
          1,
          TextureUsage::Sampled | TextureUsage::Storage | TextureUsage::RenderTarget,
          8 },
        { "frame.ldr", TextureType::Tex2D, Format::RGBA8Unorm, { kFrameWidth, kFrameHeight, 1 }, 1, TextureUsage::RenderTarget, 4 },
        { "frame.bloom0", TextureType::Tex2D, Format::RGBA16Float, { 960, 540, 1 }, 1, kSampledColor, 8 },
        { "frame.bloom1", TextureType::Tex2D, Format::RGBA16Float, { 480, 270, 1 }, 1, kSampledColor, 8 },
    } };

    /// The spec of one frame texture.
    constexpr const TextureSpec& textureSpec(FrameTexture texture)
    {
        return kTextures[static_cast<size_t>(texture)];
    }

    // Buffers whose contents are fixed: the scene, uploaded once at creation.
    inline constexpr uint64_t kInstancesBytes = uint64_t{ kBoxCount } * sizeof(Transform);
    inline constexpr uint64_t kVerticesBytes = uint64_t{ kMeshCount } * kMeshVertexCount * sizeof(MeshVertex);
    inline constexpr uint64_t kIndicesBytes = uint64_t{ kMeshCount } * kMeshIndexCount * sizeof(uint16_t);

    inline constexpr std::string_view kInstancesLabel = "frame.instances"; ///< Gpu, no usage flag.
    inline constexpr std::string_view kVerticesLabel = "frame.vertices";   ///< Gpu, no usage flag.
    inline constexpr std::string_view kIndicesLabel = "frame.indices";     ///< Gpu, BufferUsage::Index.

    // records[s]: four views, cascades 0 to 2 then the camera. View v starts at v * kViewStride with its count (one
    // uint32) at kCountOffset and kRecordsPerView chapter 08 DrawIndexedIndirectArgs from kRecordsOffset. A view is
    // 256-byte aligned. The buffer is Gpu with BufferUsage::IndirectArgs | BufferUsage::ShaderWrite.
    inline constexpr uint32_t kRecordsPerView = 10'000;
    inline constexpr uint64_t kCountOffset = 0;
    inline constexpr uint64_t kRecordsOffset = 16;
    inline constexpr uint64_t kViewStride = 240'128; ///< 16 + 10,000 * 24 bytes, rounded up to 256.
    inline constexpr uint64_t kRecordsBytes = kViewStride * kViewCount;

    /// The byte offset of view `view`'s count in records[s].
    constexpr uint64_t viewCountOffset(uint32_t view)
    {
        return view * kViewStride + kCountOffset;
    }

    /// The byte offset of view `view`'s first record in records[s].
    constexpr uint64_t viewRecordsOffset(uint32_t view)
    {
        return view * kViewStride + kRecordsOffset;
    }

    // The camera is view 3; cascade c is view c.
    inline constexpr uint32_t kCameraView = 3;

    inline constexpr std::array<std::string_view, kFrameSlots> kRecordsLabels{ "frame.records0", "frame.records1", "frame.records2" };

    // readback[s]: a Readback buffer holding row 540 of `ldr`.
    inline constexpr uint32_t kReadbackRow = 540;
    inline constexpr uint64_t kReadbackBytes = uint64_t{ kFrameWidth } * 4;
    inline constexpr std::array<std::string_view, kFrameSlots> kReadbackLabels{ "frame.readback0", "frame.readback1", "frame.readback2" };

    // The lists, in submission order, each recorded once per frame.
    enum class FrameList : uint8_t
    {
        Cull,
        Gbuffer,
        Ao,
        Shadows,
        Lighting,
    };

    inline constexpr uint32_t kListCount = 5;

    struct ListSpec
    {
        std::string_view label;
        QueueType queue;
    };

    inline constexpr std::array<ListSpec, kListCount> kLists{ {
        { "cull", QueueType::Compute },
        { "gbuffer", QueueType::Graphics },
        { "ao", QueueType::Compute },
        { "shadows", QueueType::Graphics },
        { "lighting", QueueType::Graphics },
    } };

    /// The spec of one list; the list's label names both the command list and, in N, its allocator.
    constexpr const ListSpec& listSpec(FrameList list)
    {
        return kLists[static_cast<size_t>(list)];
    }

    // The lighting list makes one uploadFrameData of cameraViewProjection() (kCameraUploadBytes, default 256 alignment; N: one
    // arena copy at the same alignment) after pass 7's dispatch and before pass 8's barrier batch, and every ForwardRoot::camera
    // is that address.
    inline constexpr uint32_t kCameraUploadBytes = sizeof(Transform);

    // The 14 passes in order. A pass's label is the name of its debug group, its timestamp pair and, for a render pass,
    // its encoder; passes 1, 3, 7 and 14 of R are also wrapped in a beginScope/endScope of the same label.
    struct PassSpec
    {
        std::string_view label;
        FrameList list;
    };

    inline constexpr uint32_t kPassCount = 14;

    inline constexpr std::array<PassSpec, kPassCount> kPasses{ {
        { "cull", FrameList::Cull },
        { "geometry", FrameList::Gbuffer },
        { "ao", FrameList::Ao },
        { "shadow0", FrameList::Shadows },
        { "shadow1", FrameList::Shadows },
        { "shadow2", FrameList::Shadows },
        { "lighting", FrameList::Lighting },
        { "forward", FrameList::Lighting },
        { "bloomDown0", FrameList::Lighting },
        { "bloomDown1", FrameList::Lighting },
        { "bloomUp1", FrameList::Lighting },
        { "bloomUp0", FrameList::Lighting },
        { "toneMap", FrameList::Lighting },
        { "readback", FrameList::Lighting },
    } };

    // Dispatch sizes, in groups. The culling kernel has one thread per box and view; the image kernels one per pixel.
    inline constexpr Extent3D kCullGroups{ 625, 1, 1 };
    inline constexpr uint32_t kCullThreadsPerGroup = 64;
    inline constexpr Extent3D kImageGroups{ 240, 135, 1 };
    inline constexpr Extent3D kImageThreadsPerGroup{ 8, 8, 1 };

    // The forward pass's boxes are drawn one per pushRoot, directly, in index order.
    inline constexpr uint32_t kForwardDraws = kForwardBoxCount;

    // The outputs both arms dump after the last measured frame, as DIR/frame.<arm>.<name>.bin, in this order. A texture
    // is its whole base mip, rows tightly packed (see repackRows), texels in the format's own byte layout; a `shadow`
    // layer is a separate output; `readback` is readback[s] of the slot of the last measured frame; `records` is
    // packRecords of records[s]: only bytes the frame writes, so undefined bytes never reach a comparison.
    struct OutputSpec
    {
        std::string_view name;
        uint64_t bytes;
    };

    // The dump of records[s]: for each view in order, its count word (4 bytes) and then the count records it covers, every
    // byte of which the cull pass writes. The count is read from the buffer and clamped to kRecordsPerView; for a correct
    // frame it is kKeptPerView in every view, which fixes the size below.
    inline constexpr uint64_t kRecordsDumpBytes = uint64_t{ kViewCount } * (sizeof(uint32_t) + uint64_t{ kKeptPerView } * sizeof(DrawIndexedIndirectArgs));

    /// The dump bytes of a records[s] buffer read back whole: see kRecordsDumpBytes. Both arms call this on their readback.
    inline std::vector<std::byte> packRecords(std::span<const std::byte> buffer)
    {
        std::vector<std::byte> dump;
        dump.reserve(kRecordsDumpBytes);
        for (uint32_t view = 0; view < kViewCount; ++view)
        {
            uint32_t count = 0;
            std::memcpy(&count, buffer.data() + viewCountOffset(view), sizeof(count));
            dump.insert(dump.end(), buffer.begin() + viewCountOffset(view), buffer.begin() + viewCountOffset(view) + sizeof(count));
            const uint64_t bytes = uint64_t{ std::min(count, kRecordsPerView) } * sizeof(DrawIndexedIndirectArgs);
            dump.insert(dump.end(), buffer.begin() + viewRecordsOffset(view), buffer.begin() + viewRecordsOffset(view) + bytes);
        }
        return dump;
    }

    // copyTextureToBuffer needs a row pitch that is a multiple of 256 bytes (VU-rojo-cmd-0035). Both arms read a texture
    // back into a buffer of dumpRowPitch(texture) * height * layers bytes and call repackRows to drop the padding.
    // Only ao and aoRaw (1,920 bytes a row, pitch 2,048) differ from their tight row.
    constexpr uint32_t dumpRowPitch(FrameTexture texture)
    {
        const TextureSpec& spec = textureSpec(texture);
        const uint32_t row = spec.extent.width * spec.bytesPerTexel;
        return (row + 255) / 256 * 256;
    }

    /// Copies `rows` rows of `rowBytes` from a buffer whose rows are `pitch` apart into tightly packed rows.
    inline std::vector<std::byte> repackRows(std::span<const std::byte> padded, uint32_t rowBytes, uint32_t pitch, uint32_t rows)
    {
        std::vector<std::byte> packed(uint64_t{ rowBytes } * rows);
        for (uint32_t row = 0; row < rows; ++row)
        {
            std::memcpy(packed.data() + uint64_t{ row } * rowBytes, padded.data() + uint64_t{ row } * pitch, rowBytes);
        }
        return packed;
    }

    inline constexpr uint64_t kShadowLayerBytes = uint64_t{ 2048 } * 2048 * 4;

    constexpr uint64_t textureBytes(FrameTexture texture)
    {
        const TextureSpec& spec = textureSpec(texture);
        return uint64_t{ spec.extent.width } * spec.extent.height * spec.bytesPerTexel * spec.layers;
    }

    inline constexpr uint32_t kOutputCount = 15;

    inline constexpr std::array<OutputSpec, kOutputCount> kOutputs{ {
        { "albedo", textureBytes(FrameTexture::Albedo) },
        { "normal", textureBytes(FrameTexture::Normal) },
        { "material", textureBytes(FrameTexture::Material) },
        { "depth", textureBytes(FrameTexture::Depth) },
        { "aoRaw", textureBytes(FrameTexture::AoRaw) },
        { "ao", textureBytes(FrameTexture::Ao) },
        { "shadow0", kShadowLayerBytes },
        { "shadow1", kShadowLayerBytes },
        { "shadow2", kShadowLayerBytes },
        { "hdr", textureBytes(FrameTexture::Hdr) },
        { "ldr", textureBytes(FrameTexture::Ldr) },
        { "bloom0", textureBytes(FrameTexture::Bloom0) },
        { "bloom1", textureBytes(FrameTexture::Bloom1) },
        { "records", kRecordsDumpBytes },
        { "readback", kReadbackBytes },
    } };

    //==================================================================================================================
    // Every fixed value either arm must reproduce. The pre-registration fixes the load of each target (the Undefined
    // old layouts and the "clears all targets" of the geometry and shadow passes, "loaded" for forward and bloom up),
    // the reverse-Z Greater depth test with depth not written by forward, the additive blend of bloom up and the alpha
    // blend of forward. Where it is silent the value below is a choice made once for both arms: noted "chosen".

    // The one sampler every sampling pass uses (chosen): linear, clamped, no mip filtering, no anisotropy.
    inline constexpr SamplerDesc kFrameSampler{ .filter = SamplerFilter::Linear,
                                                .mipFilter = SamplerFilter::Nearest,
                                                .address = SamplerAddress::ClampToEdge,
                                                .maxAnisotropy = 1,
                                                .compareEnable = false,
                                                .label = "frame.sampler" };

    // Root scalars (chosen).
    inline constexpr float kBloomUpIntensity = 0.5f;
    inline constexpr float kToneMapExposure = 1.0f;
    // Bloom down reads a source of this size: pass bloomDown0 the 1920x1080 `hdr`, bloomDown1 the 960x540 `bloom0`.
    inline constexpr float kBloomDown0TexelX = 1.0f / 1920.0f;
    inline constexpr float kBloomDown0TexelY = 1.0f / 1080.0f;
    inline constexpr float kBloomDown1TexelX = 1.0f / 960.0f;
    inline constexpr float kBloomDown1TexelY = 1.0f / 540.0f;

    // Blending. Alpha: source alpha over, alpha channel accumulating (chosen factors for the pre-registered alpha blend).
    // Additive: source and destination added in color and alpha.
    inline constexpr BlendState kAlphaBlend{ .enable = true,
                                             .srcColor = BlendFactor::SrcAlpha,
                                             .dstColor = BlendFactor::OneMinusSrcAlpha,
                                             .colorOp = BlendOp::Add,
                                             .srcAlpha = BlendFactor::One,
                                             .dstAlpha = BlendFactor::OneMinusSrcAlpha,
                                             .alphaOp = BlendOp::Add };
    inline constexpr BlendState kAdditiveBlend{ .enable = true,
                                                .srcColor = BlendFactor::One,
                                                .dstColor = BlendFactor::One,
                                                .colorOp = BlendOp::Add,
                                                .srcAlpha = BlendFactor::One,
                                                .dstAlpha = BlendFactor::One,
                                                .alphaOp = BlendOp::Add };
    inline constexpr BlendState kNoBlend{};

    // Depth tests, reverse Z (pre-registered): Greater; forward tests without writing. No stencil anywhere.
    inline constexpr DepthStencilState kDepthWrite{ .depthTest = true, .depthWrite = true, .depthCompare = CompareOp::Greater };
    inline constexpr DepthStencilState kDepthTestOnly{ .depthTest = true, .depthWrite = false, .depthCompare = CompareOp::Greater };
    inline constexpr DepthStencilState kNoDepth{ .depthTest = false, .depthWrite = false, .depthCompare = CompareOp::Always };

    // Rasterisation (chosen): the box meshes wind counter-clockwise from outside, so the opaque passes cull back faces;
    // the translucent boxes and the full-screen triangle cull nothing. Solid fill, no depth bias, depth clip on.
    inline constexpr RasterState kCullBack{ .cull = CullMode::Back, .frontFace = FrontFace::CounterClockwise };
    inline constexpr RasterState kCullNone{ .cull = CullMode::None, .frontFace = FrontFace::CounterClockwise };

    // The attachments of a render pass. Every store is Store.
    struct ColorPassAttachment
    {
        FrameTexture texture = FrameTexture::Hdr;
        LoadOp load = LoadOp::Load;
        ClearColor clear;
    };

    struct DepthPassAttachment
    {
        FrameTexture texture = FrameTexture::Depth;
        uint32_t layer = 0;
        LoadOp load = LoadOp::Load;
        float clear = 0.0f;
    };

    // One render pass of kPasses: attachments, viewport, scissor and pipeline state. The viewport covers the target with
    // depth range 0 to 1 and the scissor and the pass area are the same rectangle.
    struct RenderPassSpec
    {
        uint32_t pass; ///< Index into kPasses.
        uint32_t colorCount;
        std::array<ColorPassAttachment, 3> colors;
        bool hasDepth;
        DepthPassAttachment depth;
        Viewport viewport;
        Rect2D scissor;
        RasterState raster;
        DepthStencilState depthStencil;
        BlendState blend; ///< Of color attachment 0; the geometry pass blends nothing.
    };

    inline constexpr Viewport kFullViewport{ 0.0f, 0.0f, 1920.0f, 1080.0f, 0.0f, 1.0f };
    inline constexpr Viewport kShadowViewport{ 0.0f, 0.0f, 2048.0f, 2048.0f, 0.0f, 1.0f };
    inline constexpr Viewport kBloom0Viewport{ 0.0f, 0.0f, 960.0f, 540.0f, 0.0f, 1.0f };
    inline constexpr Viewport kBloom1Viewport{ 0.0f, 0.0f, 480.0f, 270.0f, 0.0f, 1.0f };
    inline constexpr Rect2D kFullScissor{ 0, 0, 1920, 1080 };
    inline constexpr Rect2D kShadowScissor{ 0, 0, 2048, 2048 };
    inline constexpr Rect2D kBloom0Scissor{ 0, 0, 960, 540 };
    inline constexpr Rect2D kBloom1Scissor{ 0, 0, 480, 270 };

    // Clears (chosen): colors transparent black; depth 0, the far plane of reverse Z. The full-screen passes overwrite
    // every texel of a target they do not Load, so their load is DontCare.
    inline constexpr ClearColor kClearBlack{ 0.0f, 0.0f, 0.0f, 0.0f };
    inline constexpr float kClearDepth = 0.0f;

    inline constexpr uint32_t kRenderPassCount = 10;

    // Pass indices in kPasses of the render passes.
    inline constexpr std::array<RenderPassSpec, kRenderPassCount> kRenderPasses{ {
        // 1 geometry: albedo, normal, material, depth all cleared.
        { 1,
          3,
          { { { FrameTexture::Albedo, LoadOp::Clear, kClearBlack },
              { FrameTexture::Normal, LoadOp::Clear, kClearBlack },
              { FrameTexture::Material, LoadOp::Clear, kClearBlack } } },
          true,
          { FrameTexture::Depth, 0, LoadOp::Clear, kClearDepth },
          kFullViewport,
          kFullScissor,
          kCullBack,
          kDepthWrite,
          kNoBlend },
        // 3 to 5 shadows: layer c cleared.
        { 3, 0, {}, true, { FrameTexture::Shadow, 0, LoadOp::Clear, kClearDepth }, kShadowViewport, kShadowScissor, kCullBack, kDepthWrite, kNoBlend },
        { 4, 0, {}, true, { FrameTexture::Shadow, 1, LoadOp::Clear, kClearDepth }, kShadowViewport, kShadowScissor, kCullBack, kDepthWrite, kNoBlend },
        { 5, 0, {}, true, { FrameTexture::Shadow, 2, LoadOp::Clear, kClearDepth }, kShadowViewport, kShadowScissor, kCullBack, kDepthWrite, kNoBlend },
        // 7 forward: hdr and depth loaded, depth tested and not written, alpha blended.
        { 7,
          1,
          { { { FrameTexture::Hdr, LoadOp::Load, kClearBlack } } },
          true,
          { FrameTexture::Depth, 0, LoadOp::Load, kClearDepth },
          kFullViewport,
          kFullScissor,
          kCullNone,
          kDepthTestOnly,
          kAlphaBlend },
        // 8, 9 bloom down: the target is overwritten.
        { 8, 1, { { { FrameTexture::Bloom0, LoadOp::DontCare, kClearBlack } } }, false, {}, kBloom0Viewport, kBloom0Scissor, kCullNone, kNoDepth, kNoBlend },
        { 9, 1, { { { FrameTexture::Bloom1, LoadOp::DontCare, kClearBlack } } }, false, {}, kBloom1Viewport, kBloom1Scissor, kCullNone, kNoDepth, kNoBlend },
        // 10, 11 bloom up: the target is loaded and added to.
        { 10, 1, { { { FrameTexture::Bloom0, LoadOp::Load, kClearBlack } } }, false, {}, kBloom0Viewport, kBloom0Scissor, kCullNone, kNoDepth, kAdditiveBlend },
        { 11, 1, { { { FrameTexture::Hdr, LoadOp::Load, kClearBlack } } }, false, {}, kFullViewport, kFullScissor, kCullNone, kNoDepth, kAdditiveBlend },
        // 12 tone map: ldr overwritten.
        { 12, 1, { { { FrameTexture::Ldr, LoadOp::DontCare, kClearBlack } } }, false, {}, kFullViewport, kFullScissor, kCullNone, kNoDepth, kNoBlend },
    } };
} // namespace rojoRHI::bench
