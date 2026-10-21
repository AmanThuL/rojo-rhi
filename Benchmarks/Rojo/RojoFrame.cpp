//----------------------------------------------------------------------------------------------------------------------
/// @file RojoFrame.cpp
/// @brief Implements the composed frame's RojoRHI arm: its roots, its five lists, their submissions and the dump frame.
//----------------------------------------------------------------------------------------------------------------------
#include "Rojo/RojoFrame.h"

#include "Common/FrameArms.h"
#include "Common/Runner.h"

#include <cstddef>
#include <format>
#include <optional>

namespace rojoRHI::bench {
    namespace {
        constexpr uint64_t kDumpTimeoutNs = 60'000'000'000ull;

        // Pass indices of kPasses.
        constexpr uint32_t kCullPass = 0;
        constexpr uint32_t kGeometryPass = 1;
        constexpr uint32_t kAoPass = 2;
        constexpr uint32_t kShadow0Pass = 3;
        constexpr uint32_t kLightingPass = 6;
        constexpr uint32_t kForwardPass = 7;
        constexpr uint32_t kBloomDown0Pass = 8;
        constexpr uint32_t kBloomDown1Pass = 9;
        constexpr uint32_t kBloomUp1Pass = 10;
        constexpr uint32_t kBloomUp0Pass = 11;
        constexpr uint32_t kToneMapPass = 12;
        constexpr uint32_t kReadbackPass = 13;

        static_assert(sizeof(Transform) == kCameraUploadBytes, "the camera upload is one Transform");

        //==============================================================================================================
        // One texture barrier over every subresource, written in the pass table's order: layouts, then source and
        // destination stage and access.
        TextureBarrier transition(Texture texture, Layout from, Layout to, Stage srcStage, Access srcAccess, Stage dstStage, Access dstAccess)
        {
            return TextureBarrier{
                .texture = texture,
                .srcStage = srcStage,
                .srcAccess = srcAccess,
                .dstStage = dstStage,
                .dstAccess = dstAccess,
                .oldLayout = from,
                .newLayout = to,
            };
        }

        //==============================================================================================================
        void setRows(const Transform& transform, Float4& row0, Float4& row1, Float4& row2, Float4& row3)
        {
            row0 = transform.row0;
            row1 = transform.row1;
            row2 = transform.row2;
            row3 = transform.row3;
        }

        // One texture output of the dump: kOutputs begins with these thirteen, in this order.
        struct TextureDump
        {
            FrameTexture texture;
            uint32_t layer;
        };

        constexpr std::array<TextureDump, 13> kTextureDumps{ {
            { FrameTexture::Albedo, 0 },
            { FrameTexture::Normal, 0 },
            { FrameTexture::Material, 0 },
            { FrameTexture::Depth, 0 },
            { FrameTexture::AoRaw, 0 },
            { FrameTexture::Ao, 0 },
            { FrameTexture::Shadow, 0 },
            { FrameTexture::Shadow, 1 },
            { FrameTexture::Shadow, 2 },
            { FrameTexture::Hdr, 0 },
            { FrameTexture::Ldr, 0 },
            { FrameTexture::Bloom0, 0 },
            { FrameTexture::Bloom1, 0 },
        } };

        static_assert(kTextureDumps.size() + 2 == kOutputCount, "the outputs are the textures, then records and readback");
    } // namespace

    //==================================================================================================================
    RojoFrame::RojoFrame(RojoContext& context, const Config& /*config*/) : m_context(context), m_ready(create())
    {
    }

    //==================================================================================================================
    std::expected<void, std::string> RojoFrame::create()
    {
        std::expected<RojoFrameResources, std::string> resources = createFrameResources(m_context);
        if (!resources)
        {
            return std::unexpected(resources.error());
        }
        m_resources = *resources;

        const struct
        {
            std::string_view label;
            Fence* fence;
        } fences[] = { { "frame.cullDone", &m_cullDone }, { "frame.gbufferDone", &m_gbufferDone }, { "frame.aoDone", &m_aoDone } };
        for (const auto& [label, fence] : fences)
        {
            const std::expected<Fence, std::string> created = m_context.fence(0, label);
            if (!created)
            {
                return std::unexpected(created.error());
            }
            *fence = *created;
        }

        Device device = m_context.device();
        const Addr instances = device.address(m_resources.instances);
        const Addr vertices = device.address(m_resources.vertices);
        for (uint32_t slot = 0; slot < kFrameSlots; ++slot)
        {
            const Addr records = device.address(m_resources.records[slot]);
            m_records[slot] = records;
            m_cullRoots[slot] = CullRoot{
                .instances = instances,
                .records0 = records + viewRecordsOffset(0),
                .records1 = records + viewRecordsOffset(1),
                .records2 = records + viewRecordsOffset(2),
                .records3 = records + viewRecordsOffset(3),
                .counts0 = records + viewCountOffset(0),
                .counts1 = records + viewCountOffset(1),
                .counts2 = records + viewCountOffset(2),
                .counts3 = records + viewCountOffset(3),
                .pad0 = 0,
                .pad1 = 0,
            };
        }

        m_camera = cameraViewProjection();
        m_geometryRoot = GeometryRoot{ .instances = instances, .vertices = vertices };
        setRows(m_camera, m_geometryRoot.viewProj0, m_geometryRoot.viewProj1, m_geometryRoot.viewProj2, m_geometryRoot.viewProj3);
        for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade)
        {
            ShadowRoot& root = m_shadowRoots[cascade];
            root = ShadowRoot{ .instances = instances, .vertices = vertices };
            setRows(cascadeViewProjection(cascade), root.viewProj0, root.viewProj1, root.viewProj2, root.viewProj3);
        }

        m_aoRoot = AoRoot{ .depth = m_resources.view(FrameTexture::Depth),
                           .normal = m_resources.view(FrameTexture::Normal),
                           .aoRaw = static_cast<uint32_t>(m_resources.aoRawStorage),
                           .width = kFrameWidth,
                           .height = kFrameHeight };
        m_blurRoot = BlurRoot{
            .aoRaw = m_resources.view(FrameTexture::AoRaw), .ao = static_cast<uint32_t>(m_resources.aoStorage), .width = kFrameWidth, .height = kFrameHeight
        };

        m_lightingRoot = LightingRoot{ .albedo = m_resources.view(FrameTexture::Albedo),
                                       .normal = m_resources.view(FrameTexture::Normal),
                                       .material = m_resources.view(FrameTexture::Material),
                                       .depth = m_resources.view(FrameTexture::Depth),
                                       .shadow = m_resources.view(FrameTexture::Shadow),
                                       .ao = m_resources.view(FrameTexture::Ao),
                                       .hdr = static_cast<uint32_t>(m_resources.hdrStorage),
                                       .width = kFrameWidth,
                                       .height = kFrameHeight,
                                       .lightDirection = lightDirection() };
        LightingRoot& lighting = m_lightingRoot;
        setRows(cameraInverseViewProjection(), lighting.inverseViewProj0, lighting.inverseViewProj1, lighting.inverseViewProj2, lighting.inverseViewProj3);
        setRows(cascadeViewProjection(0), lighting.cascade0Row0, lighting.cascade0Row1, lighting.cascade0Row2, lighting.cascade0Row3);
        setRows(cascadeViewProjection(1), lighting.cascade1Row0, lighting.cascade1Row1, lighting.cascade1Row2, lighting.cascade1Row3);
        setRows(cascadeViewProjection(2), lighting.cascade2Row0, lighting.cascade2Row1, lighting.cascade2Row2, lighting.cascade2Row3);

        m_forwardRoots.reserve(kForwardDraws);
        m_forwardMeshes.reserve(kForwardDraws);
        for (uint32_t index = 0; index < kForwardDraws; ++index)
        {
            const ForwardBox box = forwardBox(index);
            const uint32_t mesh = box.box % kMeshCount;
            ForwardRoot root{ .color = box.color, .vertices = vertices + uint64_t{ kMeshVertexCount } * mesh * sizeof(MeshVertex) };
            setRows(box.transform, root.transform0, root.transform1, root.transform2, root.transform3);
            m_forwardRoots.push_back(root);
            m_forwardMeshes.push_back(mesh);
        }

        const uint32_t sampler = static_cast<uint32_t>(m_resources.sampler);
        m_bloomDown0Root =
            BloomDownRoot{ .source = m_resources.view(FrameTexture::Hdr), .samplerId = sampler, .texelX = kBloomDown0TexelX, .texelY = kBloomDown0TexelY };
        m_bloomDown1Root =
            BloomDownRoot{ .source = m_resources.view(FrameTexture::Bloom0), .samplerId = sampler, .texelX = kBloomDown1TexelX, .texelY = kBloomDown1TexelY };
        m_bloomUp1Root = BloomUpRoot{ .source = m_resources.view(FrameTexture::Bloom1), .samplerId = sampler, .intensity = kBloomUpIntensity, .pad0 = 0 };
        m_bloomUp0Root = BloomUpRoot{ .source = m_resources.view(FrameTexture::Bloom0), .samplerId = sampler, .intensity = kBloomUpIntensity, .pad0 = 0 };
        m_toneMapRoot = ToneMapRoot{ .source = m_resources.view(FrameTexture::Hdr), .samplerId = sampler, .exposure = kToneMapExposure, .pad0 = 0 };
        return {};
    }

    //==================================================================================================================
    const std::expected<void, std::string>& RojoFrame::ready() const
    {
        return m_ready;
    }

    //==================================================================================================================
    // The five lists are acquired up front and recorded in submission order; a submission completes inside the call on
    // Null, and endFrame waits for the five commits' reports on Metal 4.
    std::expected<FrameResult, std::string> RojoFrame::frame(uint32_t index)
    {
        const uint32_t slot = slotOf(index);
        const uint64_t number = uint64_t{ index } + 1;
        m_lastFrame = index;

        m_context.beginFrame(kListCount);
        const Device device = m_context.device();
        std::array<List, kListCount> lists;
        for (uint32_t list = 0; list < kListCount; ++list)
        {
            lists[list] = acquire(device, kLists[list].queue, kLists[list].label);
        }
        List& cull = lists[static_cast<size_t>(FrameList::Cull)];
        List& gbuffer = lists[static_cast<size_t>(FrameList::Gbuffer)];
        List& ao = lists[static_cast<size_t>(FrameList::Ao)];
        List& shadows = lists[static_cast<size_t>(FrameList::Shadows)];
        List& lighting = lists[static_cast<size_t>(FrameList::Lighting)];

        recordCull(cull, slot);
        cull.end();
        recordGbuffer(gbuffer, slot);
        gbuffer.end();
        recordAo(ao);
        ao.end();
        recordShadows(shadows, slot);
        shadows.end();
        recordLighting(lighting, slot);
        lighting.end();

        // The submission table: the occlusion overlaps the shadow passes, and the cascades read records[s] behind the
        // wait of the gbuffer submission.
        const FenceValue cullDone{ m_cullDone, number };
        const FenceValue gbufferDone{ m_gbufferDone, number };
        const FenceValue aoDone{ m_aoDone, number };
        QueueRef compute = m_context.queue(QueueType::Compute);
        QueueRef graphics = m_context.queue(QueueType::Graphics);
        compute.submit(std::span(&cull, 1), {}, std::span(&cullDone, 1));
        graphics.submit(std::span(&gbuffer, 1), std::span(&cullDone, 1), std::span(&gbufferDone, 1));
        compute.submit(std::span(&ao, 1), std::span(&gbufferDone, 1), std::span(&aoDone, 1));
        graphics.submit(std::span(&shadows, 1));
        graphics.submit(std::span(&lighting, 1), std::span(&aoDone, 1));

        const std::expected<std::optional<double>, std::string> gpu = m_context.endFrame();
        if (!gpu)
        {
            return std::unexpected(gpu.error());
        }
        return FrameResult{ .cpuNs = std::nullopt, .gpuNs = *gpu };
    }

    //==================================================================================================================
    // Pass 1, cull.
    void RojoFrame::recordCull(List& list, uint32_t slot)
    {
        list.beginScope(kPasses[kCullPass].label);
        list.setPipeline(m_resources.cull);
        list.pushRoot(m_cullRoots[slot]);
        list.dispatch(kCullGroups.width, kCullGroups.height, kCullGroups.depth);
        list.endScope();
    }

    //==================================================================================================================
    // Pass 2, geometry, then the batch that hands the G-buffer to the occlusion and lighting kernels.
    void RojoFrame::recordGbuffer(List& list, uint32_t slot)
    {
        // albedo, normal, material: Undefined→ColorAttachment, ComputeShader/None → ColorAttachment/ColorAttachmentWrite;
        // depth: Undefined→DepthStencilWrite, DepthStencil/None → DepthStencil/DepthStencilRead+DepthStencilWrite. Row 2 of
        // the pass table ends at both accesses because the pass tests and writes depth, which chapter 09 counts as both.
        const std::array<TextureBarrier, 4> before{
            transition(
                m_resources.texture(FrameTexture::Albedo),
                Layout::Undefined,
                Layout::ColorAttachment,
                Stage::ComputeShader,
                Access::None,
                Stage::ColorAttachment,
                Access::ColorAttachmentWrite
            ),
            transition(
                m_resources.texture(FrameTexture::Normal),
                Layout::Undefined,
                Layout::ColorAttachment,
                Stage::ComputeShader,
                Access::None,
                Stage::ColorAttachment,
                Access::ColorAttachmentWrite
            ),
            transition(
                m_resources.texture(FrameTexture::Material),
                Layout::Undefined,
                Layout::ColorAttachment,
                Stage::ComputeShader,
                Access::None,
                Stage::ColorAttachment,
                Access::ColorAttachmentWrite
            ),
            transition(
                m_resources.texture(FrameTexture::Depth),
                Layout::Undefined,
                Layout::DepthStencilWrite,
                Stage::DepthStencil,
                Access::None,
                Stage::DepthStencil,
                Access::DepthStencilRead | Access::DepthStencilWrite
            ),
        };
        list.barrier({}, {}, before);
        beginPass(list, kGeometryPass, m_resources.geometry);
        list.pushRoot(m_geometryRoot);
        list.drawIndexedIndirect(
            m_resources.indices,
            IndexType::UInt16,
            m_records[slot] + viewRecordsOffset(kCameraView),
            kRecordsPerView,
            m_records[slot] + viewCountOffset(kCameraView)
        );
        list.endRenderPass();

        // albedo, normal, material: ColorAttachment→ShaderRead, ColorAttachment/ColorAttachmentWrite → ComputeShader/ShaderRead;
        // depth: DepthStencilWrite→ShaderRead, DepthStencil/DepthStencilWrite → ComputeShader/ShaderRead.
        const std::array<TextureBarrier, 4> after{
            transition(
                m_resources.texture(FrameTexture::Albedo),
                Layout::ColorAttachment,
                Layout::ShaderRead,
                Stage::ColorAttachment,
                Access::ColorAttachmentWrite,
                Stage::ComputeShader,
                Access::ShaderRead
            ),
            transition(
                m_resources.texture(FrameTexture::Normal),
                Layout::ColorAttachment,
                Layout::ShaderRead,
                Stage::ColorAttachment,
                Access::ColorAttachmentWrite,
                Stage::ComputeShader,
                Access::ShaderRead
            ),
            transition(
                m_resources.texture(FrameTexture::Material),
                Layout::ColorAttachment,
                Layout::ShaderRead,
                Stage::ColorAttachment,
                Access::ColorAttachmentWrite,
                Stage::ComputeShader,
                Access::ShaderRead
            ),
            transition(
                m_resources.texture(FrameTexture::Depth),
                Layout::DepthStencilWrite,
                Layout::ShaderRead,
                Stage::DepthStencil,
                Access::DepthStencilWrite,
                Stage::ComputeShader,
                Access::ShaderRead
            ),
        };
        list.barrier({}, {}, after);
    }

    //==================================================================================================================
    // Pass 3, ao: the occlusion kernel, the barrier between it and the blur, the blur, then the batch that hands `ao` to
    // the lighting kernel.
    void RojoFrame::recordAo(List& list)
    {
        // aoRaw, ao: Undefined→ShaderWrite, None/None → ComputeShader/ShaderWrite.
        const std::array<TextureBarrier, 2> before{
            transition(
                m_resources.texture(FrameTexture::AoRaw),
                Layout::Undefined,
                Layout::ShaderWrite,
                Stage::None,
                Access::None,
                Stage::ComputeShader,
                Access::ShaderWrite
            ),
            transition(
                m_resources.texture(FrameTexture::Ao),
                Layout::Undefined,
                Layout::ShaderWrite,
                Stage::None,
                Access::None,
                Stage::ComputeShader,
                Access::ShaderWrite
            ),
        };
        // aoRaw: ShaderWrite→ShaderRead, ComputeShader/ShaderWrite → ComputeShader/ShaderRead.
        const std::array<TextureBarrier, 1> between{ transition(
            m_resources.texture(FrameTexture::AoRaw),
            Layout::ShaderWrite,
            Layout::ShaderRead,
            Stage::ComputeShader,
            Access::ShaderWrite,
            Stage::ComputeShader,
            Access::ShaderRead
        ) };
        // ao: ShaderWrite→ShaderRead, ComputeShader/ShaderWrite → ComputeShader/ShaderRead.
        const std::array<TextureBarrier, 1> after{ transition(
            m_resources.texture(FrameTexture::Ao),
            Layout::ShaderWrite,
            Layout::ShaderRead,
            Stage::ComputeShader,
            Access::ShaderWrite,
            Stage::ComputeShader,
            Access::ShaderRead
        ) };

        list.barrier({}, {}, before);
        list.beginScope(kPasses[kAoPass].label);
        list.setPipeline(m_resources.ao);
        list.pushRoot(m_aoRoot);
        list.dispatch(kImageGroups.width, kImageGroups.height, kImageGroups.depth);
        list.barrier({}, {}, between);
        list.setPipeline(m_resources.blur);
        list.pushRoot(m_blurRoot);
        list.dispatch(kImageGroups.width, kImageGroups.height, kImageGroups.depth);
        list.endScope();
        list.barrier({}, {}, after);
    }

    //==================================================================================================================
    // Passes 4 to 6, shadow0 to shadow2, each into its layer of `shadow`.
    void RojoFrame::recordShadows(List& list, uint32_t slot)
    {
        // Before pass 4 only. shadow: Undefined→DepthStencilWrite, ComputeShader/None →
        // DepthStencil/DepthStencilRead+DepthStencilWrite. Row 4 of the pass table ends at both accesses because each
        // cascade tests and writes depth, which chapter 09 counts as both.
        const std::array<TextureBarrier, 1> before{ transition(
            m_resources.texture(FrameTexture::Shadow),
            Layout::Undefined,
            Layout::DepthStencilWrite,
            Stage::ComputeShader,
            Access::None,
            Stage::DepthStencil,
            Access::DepthStencilRead | Access::DepthStencilWrite
        ) };
        list.barrier({}, {}, before);
        for (uint32_t cascade = 0; cascade < kCascadeCount; ++cascade)
        {
            beginPass(list, kShadow0Pass + cascade, m_resources.shadow);
            list.pushRoot(m_shadowRoots[cascade]);
            list.drawIndexedIndirect(
                m_resources.indices,
                IndexType::UInt16,
                m_records[slot] + viewRecordsOffset(cascade),
                kRecordsPerView,
                m_records[slot] + viewCountOffset(cascade)
            );
            list.endRenderPass();
        }
    }

    //==================================================================================================================
    // Passes 7 to 14: lighting, forward, the bloom chain, the tone map and the readback of row 540.
    void RojoFrame::recordLighting(List& list, uint32_t slot)
    {
        const Texture shadow = m_resources.texture(FrameTexture::Shadow);
        const Texture depth = m_resources.texture(FrameTexture::Depth);
        const Texture hdr = m_resources.texture(FrameTexture::Hdr);
        const Texture bloom0 = m_resources.texture(FrameTexture::Bloom0);
        const Texture bloom1 = m_resources.texture(FrameTexture::Bloom1);
        const Texture ldr = m_resources.texture(FrameTexture::Ldr);
        constexpr Access kColorReadWrite = Access::ColorAttachmentRead | Access::ColorAttachmentWrite;

        // 7 lighting. shadow: DepthStencilWrite→ShaderRead, DepthStencil/DepthStencilWrite → ComputeShader/ShaderRead;
        // hdr: Undefined→ShaderWrite, FragmentShader/None → ComputeShader/ShaderWrite.
        const std::array<TextureBarrier, 2> lightingBatch{
            transition(
                shadow, Layout::DepthStencilWrite, Layout::ShaderRead, Stage::DepthStencil, Access::DepthStencilWrite, Stage::ComputeShader, Access::ShaderRead
            ),
            transition(hdr, Layout::Undefined, Layout::ShaderWrite, Stage::FragmentShader, Access::None, Stage::ComputeShader, Access::ShaderWrite),
        };
        list.barrier({}, {}, lightingBatch);
        list.beginScope(kPasses[kLightingPass].label);
        list.setPipeline(m_resources.lighting);
        list.pushRoot(m_lightingRoot);
        list.dispatch(kImageGroups.width, kImageGroups.height, kImageGroups.depth);
        list.endScope();

        // The camera's view-projection, which every forward box reads through its root.
        const Addr camera = list.uploadFrameData(std::as_bytes(std::span(&m_camera, 1)));

        // 8 forward. hdr: ShaderWrite→ColorAttachment, ComputeShader/ShaderWrite → ColorAttachment/ColorAttachmentRead+ColorAttachmentWrite;
        // depth: ShaderRead→DepthStencilWrite, ComputeShader/None → DepthStencil/DepthStencilRead.
        const std::array<TextureBarrier, 2> forwardBatch{
            transition(hdr, Layout::ShaderWrite, Layout::ColorAttachment, Stage::ComputeShader, Access::ShaderWrite, Stage::ColorAttachment, kColorReadWrite),
            transition(depth, Layout::ShaderRead, Layout::DepthStencilWrite, Stage::ComputeShader, Access::None, Stage::DepthStencil, Access::DepthStencilRead),
        };
        list.barrier({}, {}, forwardBatch);
        beginPass(list, kForwardPass, m_resources.forward);
        for (uint32_t box = 0; box < kForwardDraws; ++box)
        {
            ForwardRoot root = m_forwardRoots[box];
            root.camera = camera;
            list.pushRoot(root);
            const uint32_t mesh = m_forwardMeshes[box];
            list.drawIndexed(
                m_resources.indices, IndexType::UInt16, kMeshIndexCount, 1, kMeshIndexCount * mesh, static_cast<int32_t>(kMeshVertexCount * mesh), 0
            );
        }
        list.endRenderPass();

        // 9 bloomDown0. hdr: ColorAttachment→ShaderRead, ColorAttachment/ColorAttachmentWrite → FragmentShader/ShaderRead;
        // bloom0: Undefined→ColorAttachment, FragmentShader/None → ColorAttachment/ColorAttachmentWrite.
        const std::array<TextureBarrier, 2> bloomDown0Batch{
            transition(
                hdr,
                Layout::ColorAttachment,
                Layout::ShaderRead,
                Stage::ColorAttachment,
                Access::ColorAttachmentWrite,
                Stage::FragmentShader,
                Access::ShaderRead
            ),
            transition(
                bloom0, Layout::Undefined, Layout::ColorAttachment, Stage::FragmentShader, Access::None, Stage::ColorAttachment, Access::ColorAttachmentWrite
            ),
        };
        fullScreenPass(list, kBloomDown0Pass, m_resources.bloomDown, bloomDown0Batch, m_bloomDown0Root);

        // 10 bloomDown1. bloom0: ColorAttachment→ShaderRead, ColorAttachment/ColorAttachmentWrite → FragmentShader/ShaderRead;
        // bloom1: Undefined→ColorAttachment, FragmentShader/None → ColorAttachment/ColorAttachmentWrite.
        const std::array<TextureBarrier, 2> bloomDown1Batch{
            transition(
                bloom0,
                Layout::ColorAttachment,
                Layout::ShaderRead,
                Stage::ColorAttachment,
                Access::ColorAttachmentWrite,
                Stage::FragmentShader,
                Access::ShaderRead
            ),
            transition(
                bloom1, Layout::Undefined, Layout::ColorAttachment, Stage::FragmentShader, Access::None, Stage::ColorAttachment, Access::ColorAttachmentWrite
            ),
        };
        fullScreenPass(list, kBloomDown1Pass, m_resources.bloomDown, bloomDown1Batch, m_bloomDown1Root);

        // 11 bloomUp1. bloom1: ColorAttachment→ShaderRead, ColorAttachment/ColorAttachmentWrite → FragmentShader/ShaderRead;
        // bloom0: ShaderRead→ColorAttachment, FragmentShader/None → ColorAttachment/ColorAttachmentRead+ColorAttachmentWrite.
        const std::array<TextureBarrier, 2> bloomUp1Batch{
            transition(
                bloom1,
                Layout::ColorAttachment,
                Layout::ShaderRead,
                Stage::ColorAttachment,
                Access::ColorAttachmentWrite,
                Stage::FragmentShader,
                Access::ShaderRead
            ),
            transition(bloom0, Layout::ShaderRead, Layout::ColorAttachment, Stage::FragmentShader, Access::None, Stage::ColorAttachment, kColorReadWrite),
        };
        fullScreenPass(list, kBloomUp1Pass, m_resources.bloomUp, bloomUp1Batch, m_bloomUp1Root);

        // 12 bloomUp0. bloom0: ColorAttachment→ShaderRead, ColorAttachment/ColorAttachmentWrite → FragmentShader/ShaderRead;
        // hdr: ShaderRead→ColorAttachment, FragmentShader/None → ColorAttachment/ColorAttachmentRead+ColorAttachmentWrite.
        const std::array<TextureBarrier, 2> bloomUp0Batch{
            transition(
                bloom0,
                Layout::ColorAttachment,
                Layout::ShaderRead,
                Stage::ColorAttachment,
                Access::ColorAttachmentWrite,
                Stage::FragmentShader,
                Access::ShaderRead
            ),
            transition(hdr, Layout::ShaderRead, Layout::ColorAttachment, Stage::FragmentShader, Access::None, Stage::ColorAttachment, kColorReadWrite),
        };
        fullScreenPass(list, kBloomUp0Pass, m_resources.bloomUp, bloomUp0Batch, m_bloomUp0Root);

        // 13 toneMap. hdr: ColorAttachment→ShaderRead, ColorAttachment/ColorAttachmentWrite → FragmentShader/ShaderRead;
        // ldr: Undefined→ColorAttachment, Copy/None → ColorAttachment/ColorAttachmentWrite.
        const std::array<TextureBarrier, 2> toneMapBatch{
            transition(
                hdr,
                Layout::ColorAttachment,
                Layout::ShaderRead,
                Stage::ColorAttachment,
                Access::ColorAttachmentWrite,
                Stage::FragmentShader,
                Access::ShaderRead
            ),
            transition(ldr, Layout::Undefined, Layout::ColorAttachment, Stage::Copy, Access::None, Stage::ColorAttachment, Access::ColorAttachmentWrite),
        };
        fullScreenPass(list, kToneMapPass, m_resources.toneMap, toneMapBatch, m_toneMapRoot);

        // 14 readback. ldr: ColorAttachment→CopySrc, ColorAttachment/ColorAttachmentWrite → Copy/CopyRead.
        const std::array<TextureBarrier, 1> readbackBatch{
            transition(ldr, Layout::ColorAttachment, Layout::CopySrc, Stage::ColorAttachment, Access::ColorAttachmentWrite, Stage::Copy, Access::CopyRead),
        };
        list.barrier({}, {}, readbackBatch);
        list.beginScope(kPasses[kReadbackPass].label);
        list.copyTextureToBuffer(
            ldr,
            TextureRegion{ .offset = { 0, kReadbackRow, 0 }, .extent = { kFrameWidth, 1, 1 } },
            m_resources.readback[slot],
            BufferTextureLayout{ .rowPitch = static_cast<uint32_t>(kReadbackBytes) }
        );
        list.endScope();
    }

    //==================================================================================================================
    void RojoFrame::beginPass(List& list, uint32_t pass, Pipeline pipeline)
    {
        const RenderPassSpec& spec = renderPassSpec(pass);
        std::array<ColorAttachment, 3> colors{};
        for (uint32_t color = 0; color < spec.colorCount; ++color)
        {
            const ColorPassAttachment& attachment = spec.colors[color];
            colors[color] = ColorAttachment{
                .texture = m_resources.texture(attachment.texture), .load = attachment.load, .store = StoreOp::Store, .clear = attachment.clear
            };
        }
        DepthAttachment depth{};
        if (spec.hasDepth)
        {
            depth = DepthAttachment{ .texture = m_resources.texture(spec.depth.texture),
                                     .layer = spec.depth.layer,
                                     .load = spec.depth.load,
                                     .store = StoreOp::Store,
                                     .clearDepth = spec.depth.clear };
        }
        list.beginRenderPass(
            RenderPassDesc{ .colors = std::span(colors.data(), spec.colorCount), .depth = depth, .area = spec.scissor, .label = kPasses[pass].label }
        );
        list.setPipeline(pipeline);
        list.setViewport(spec.viewport);
        list.setScissor(spec.scissor);
    }

    //==================================================================================================================
    template<class Root> void RojoFrame::fullScreenPass(List& list, uint32_t pass, Pipeline pipeline, std::span<const TextureBarrier> batch, const Root& root)
    {
        list.barrier({}, {}, batch);
        beginPass(list, pass, pipeline);
        list.pushRoot(root);
        list.draw(3);
        list.endRenderPass();
    }

    //==================================================================================================================
    // Outside every measurement. Each texture moves from the layout a frame leaves it in to CopySrc (`ldr` is there
    // already), with the stage of its last use in the frame as the source, and is copied whole into a Readback buffer
    // at a 256-byte row pitch; records[s] is copied whole. The fence wait orders the CPU reads after the copies, and
    // readback[s] after the frame's own row copy.
    std::expected<std::vector<Output>, std::string> RojoFrame::readback()
    {
        const uint32_t slot = slotOf(m_lastFrame);
        const std::expected<Fence, std::string> fence = m_context.fence(0, "frame.dump");
        if (!fence)
        {
            return std::unexpected(fence.error());
        }

        std::array<Buffer, kTextureDumps.size()> textureBuffers{};
        for (size_t index = 0; index < kTextureDumps.size(); ++index)
        {
            const TextureSpec& spec = textureSpec(kTextureDumps[index].texture);
            const std::string label = std::format("frame.dump.{}", kOutputs[index].name);
            const std::expected<Buffer, std::string> buffer = m_context.buffer(
                BufferDesc{ .size = uint64_t{ dumpRowPitch(kTextureDumps[index].texture) } * spec.extent.height, .memory = Memory::Readback, .label = label }
            );
            if (!buffer)
            {
                return std::unexpected(buffer.error());
            }
            textureBuffers[index] = *buffer;
        }
        const std::expected<Buffer, std::string> recordsBuffer =
            m_context.buffer(BufferDesc{ .size = kRecordsBytes, .memory = Memory::Readback, .label = "frame.dump.records" });
        if (!recordsBuffer)
        {
            return std::unexpected(recordsBuffer.error());
        }

        const auto toCopy = [this](FrameTexture texture, Layout from, Stage lastUse) {
            return transition(m_resources.texture(texture), from, Layout::CopySrc, lastUse, Access::None, Stage::Copy, Access::CopyRead);
        };
        const std::array<TextureBarrier, 10> batch{
            toCopy(FrameTexture::Albedo, Layout::ShaderRead, Stage::ComputeShader),
            toCopy(FrameTexture::Normal, Layout::ShaderRead, Stage::ComputeShader),
            toCopy(FrameTexture::Material, Layout::ShaderRead, Stage::ComputeShader),
            toCopy(FrameTexture::Depth, Layout::DepthStencilWrite, Stage::DepthStencil),
            toCopy(FrameTexture::AoRaw, Layout::ShaderRead, Stage::ComputeShader),
            toCopy(FrameTexture::Ao, Layout::ShaderRead, Stage::ComputeShader),
            toCopy(FrameTexture::Shadow, Layout::ShaderRead, Stage::ComputeShader),
            toCopy(FrameTexture::Hdr, Layout::ShaderRead, Stage::FragmentShader),
            toCopy(FrameTexture::Bloom0, Layout::ShaderRead, Stage::FragmentShader),
            toCopy(FrameTexture::Bloom1, Layout::ShaderRead, Stage::FragmentShader),
        };

        m_context.beginFrame(1);
        List list = acquire(m_context.device(), QueueType::Graphics, "frame.dump");
        list.barrier({}, {}, batch);
        for (size_t index = 0; index < kTextureDumps.size(); ++index)
        {
            const TextureDump& dump = kTextureDumps[index];
            const TextureSpec& spec = textureSpec(dump.texture);
            list.copyTextureToBuffer(
                m_resources.texture(dump.texture),
                TextureRegion{ .layer = dump.layer, .extent = { spec.extent.width, spec.extent.height, 1 } },
                textureBuffers[index],
                BufferTextureLayout{ .rowPitch = dumpRowPitch(dump.texture) }
            );
        }
        list.copyBuffer(m_resources.records[slot], 0, *recordsBuffer, 0, kRecordsBytes);
        list.end();
        const std::array<List, 1> lists{ list };
        const std::array<FenceValue, 1> signal{ FenceValue{ *fence, 1 } };
        m_context.queue(QueueType::Graphics).submit(lists, {}, signal);
        const std::expected<std::optional<double>, std::string> ended = m_context.endFrame();
        if (!ended)
        {
            return std::unexpected("dump: " + ended.error());
        }
        if (!m_context.device().wait(signal[0], kDumpTimeoutNs))
        {
            return std::unexpected(std::string("dump: the fence did not signal within 60 s"));
        }

        Device device = m_context.device();
        std::vector<Output> outputs;
        outputs.reserve(kOutputCount);
        for (size_t index = 0; index < kTextureDumps.size(); ++index)
        {
            const FrameTexture texture = kTextureDumps[index].texture;
            const TextureSpec& spec = textureSpec(texture);
            const uint32_t pitch = dumpRowPitch(texture);
            const std::span<const std::byte> padded(device.cpuPointer(textureBuffers[index]), uint64_t{ pitch } * spec.extent.height);
            outputs.push_back(
                Output{ std::string(kOutputs[index].name), repackRows(padded, spec.extent.width * spec.bytesPerTexel, pitch, spec.extent.height) }
            );
        }
        outputs.push_back(Output{ "records", packRecords(std::span<const std::byte>(device.cpuPointer(*recordsBuffer), kRecordsBytes)) });
        const std::byte* row = device.cpuPointer(m_resources.readback[slot]);
        outputs.push_back(Output{ "readback", std::vector<std::byte>(row, row + kReadbackBytes) });
        return outputs;
    }

    //==================================================================================================================
    ArmResult runRojoFrame(const Config& config, const RunOptions& options)
    {
        // N writes a debug group and a pair of timestamps per pass, so R times its passes and scopes too.
        RunOptions timed = options;
        timed.scopeTimings = true;
        return runArm<RojoFrame>(config, timed);
    }
} // namespace rojoRHI::bench
