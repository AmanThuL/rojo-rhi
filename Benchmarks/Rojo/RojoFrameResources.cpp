//----------------------------------------------------------------------------------------------------------------------
/// @file RojoFrameResources.cpp
/// @brief Creates the composed frame's RojoRHI buffers, textures, views, sampler and pipelines.
//----------------------------------------------------------------------------------------------------------------------
#include "Rojo/RojoFrameResources.h"

#include "Common/Scene.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <span>
#include <string_view>
#include <vector>

namespace rojoRHI::bench {
    namespace {
        // Pass indices of kPasses whose render pass fixes a pipeline's state.
        constexpr uint32_t kGeometryPass = 1;
        constexpr uint32_t kShadow0Pass = 3;
        constexpr uint32_t kForwardPass = 7;
        constexpr uint32_t kBloomDown0Pass = 8;
        constexpr uint32_t kBloomUp1Pass = 10;
        constexpr uint32_t kToneMapPass = 12;

        //==============================================================================================================
        template<class T> std::span<const std::byte> bytesOf(const std::vector<T>& values)
        {
            return std::as_bytes(std::span(values));
        }

        //==============================================================================================================
        // A graphics pipeline with the attachments, rasterisation, depth state and blend of one render pass.
        std::expected<Pipeline, std::string> graphicsPipeline(
            RojoContext& context, uint32_t pass, ShaderEntry vertex, ShaderEntry fragment, std::string_view label
        )
        {
            const RenderPassSpec& spec = renderPassSpec(pass);
            std::array<ColorTarget, 3> colors{};
            for (uint32_t color = 0; color < spec.colorCount; ++color)
            {
                colors[color] = ColorTarget{ .format = textureSpec(spec.colors[color].texture).format, .blend = color == 0 ? spec.blend : kNoBlend };
            }
            return context.pipeline(
                GraphicsPipelineDesc{
                    .vertex = vertex,
                    .fragment = fragment,
                    .primitive = PrimitiveClass::Triangle,
                    .raster = spec.raster,
                    .depthStencil = spec.depthStencil,
                    .colors = std::span(colors.data(), spec.colorCount),
                    .depthFormat = spec.hasDepth ? textureSpec(spec.depth.texture).format : Format::Undefined,
                    .label = label,
                }
            );
        }

        //==============================================================================================================
        std::expected<void, std::string> createBuffers(RojoContext& context, RojoFrameResources& resources)
        {
            const std::vector<Transform> instances = allInstances();
            const std::vector<MeshVertex> vertices = allVertices();
            const std::vector<uint16_t> indices = allIndices();
            const std::expected<Buffer, std::string> instanceBuffer =
                context.buffer(BufferDesc{ .size = kInstancesBytes, .memory = Memory::Gpu, .label = kInstancesLabel }, bytesOf(instances));
            if (!instanceBuffer)
            {
                return std::unexpected(instanceBuffer.error());
            }
            resources.instances = *instanceBuffer;
            const std::expected<Buffer, std::string> vertexBuffer =
                context.buffer(BufferDesc{ .size = kVerticesBytes, .memory = Memory::Gpu, .label = kVerticesLabel }, bytesOf(vertices));
            if (!vertexBuffer)
            {
                return std::unexpected(vertexBuffer.error());
            }
            resources.vertices = *vertexBuffer;
            const std::expected<Buffer, std::string> indexBuffer = context.buffer(
                BufferDesc{ .size = kIndicesBytes, .memory = Memory::Gpu, .usage = BufferUsage::Index, .label = kIndicesLabel }, bytesOf(indices)
            );
            if (!indexBuffer)
            {
                return std::unexpected(indexBuffer.error());
            }
            resources.indices = *indexBuffer;

            for (uint32_t slot = 0; slot < kFrameSlots; ++slot)
            {
                const std::expected<Buffer, std::string> records = context.buffer(
                    BufferDesc{ .size = kRecordsBytes,
                                .memory = Memory::Gpu,
                                .usage = BufferUsage::IndirectArgs | BufferUsage::ShaderWrite,
                                .label = kRecordsLabels[slot] }
                );
                if (!records)
                {
                    return std::unexpected(records.error());
                }
                resources.records[slot] = *records;
                const std::expected<Buffer, std::string> readback =
                    context.buffer(BufferDesc{ .size = kReadbackBytes, .memory = Memory::Readback, .label = kReadbackLabels[slot] });
                if (!readback)
                {
                    return std::unexpected(readback.error());
                }
                resources.readback[slot] = *readback;
            }
            return {};
        }

        //==============================================================================================================
        std::expected<void, std::string> createTextures(RojoContext& context, RojoFrameResources& resources)
        {
            for (uint32_t index = 0; index < kTextureCount; ++index)
            {
                const TextureSpec& spec = kTextures[index];
                const std::expected<Texture, std::string> texture = context.texture(
                    TextureDesc{
                        .type = spec.type, .format = spec.format, .extent = spec.extent, .layers = spec.layers, .usage = spec.usage, .label = spec.label }
                );
                if (!texture)
                {
                    return std::unexpected(texture.error());
                }
                resources.textures[index] = *texture;
                if ((spec.usage & TextureUsage::Sampled) == TextureUsage::None)
                {
                    continue;
                }
                const bool depth = spec.format == Format::D32Float;
                const std::expected<ViewIndex, std::string> view = context.view(
                    *texture,
                    TextureViewDesc{ .type = spec.type, .usage = TextureUsage::Sampled, .aspect = depth ? TextureAspect::Depth : TextureAspect::Color }
                );
                if (!view)
                {
                    return std::unexpected(std::format("{}: {}", spec.label, view.error()));
                }
                resources.sampled[index] = *view;
            }

            const struct
            {
                FrameTexture texture;
                ViewIndex* view;
            } storage[] = { { FrameTexture::AoRaw, &resources.aoRawStorage },
                            { FrameTexture::Ao, &resources.aoStorage },
                            { FrameTexture::Hdr, &resources.hdrStorage } };
            for (const auto& [texture, view] : storage)
            {
                const std::expected<ViewIndex, std::string> created =
                    context.view(resources.texture(texture), TextureViewDesc{ .type = TextureType::Tex2D, .usage = TextureUsage::Storage });
                if (!created)
                {
                    return std::unexpected(std::format("{}: {}", textureSpec(texture).label, created.error()));
                }
                *view = *created;
            }

            const std::expected<SamplerIndex, std::string> sampler = context.sampler(kFrameSampler);
            if (!sampler)
            {
                return std::unexpected(sampler.error());
            }
            resources.sampler = *sampler;
            return {};
        }

        //==============================================================================================================
        std::expected<void, std::string> createComputePipelines(RojoContext& context, RojoFrameResources& resources)
        {
            const std::array<ShaderEntryName, 1> cullEntries{ ShaderEntryName{ "cullMain", "compute" } };
            const std::expected<ShaderLibrary, std::string> cullLibrary = context.library("BenchCull", cullEntries);
            if (!cullLibrary)
            {
                return std::unexpected(cullLibrary.error());
            }
            const std::array<ShaderEntryName, 2> aoEntries{ ShaderEntryName{ "aoMain", "compute" }, ShaderEntryName{ "aoBlurMain", "compute" } };
            const std::expected<ShaderLibrary, std::string> aoLibrary = context.library("BenchAo", aoEntries);
            if (!aoLibrary)
            {
                return std::unexpected(aoLibrary.error());
            }
            const std::array<ShaderEntryName, 1> lightingEntries{ ShaderEntryName{ "lightingMain", "compute" } };
            const std::expected<ShaderLibrary, std::string> lightingLibrary = context.library("BenchLighting", lightingEntries);
            if (!lightingLibrary)
            {
                return std::unexpected(lightingLibrary.error());
            }

            const struct
            {
                ShaderEntry entry;
                std::string_view label;
                Pipeline* pipeline;
            } kernels[] = {
                { { *cullLibrary, "cullMain" }, "frame.cull", &resources.cull },
                { { *aoLibrary, "aoMain" }, "frame.ao", &resources.ao },
                { { *aoLibrary, "aoBlurMain" }, "frame.blur", &resources.blur },
                { { *lightingLibrary, "lightingMain" }, "frame.lighting", &resources.lighting },
            };
            for (const auto& [entry, label, pipeline] : kernels)
            {
                const std::expected<Pipeline, std::string> created = context.pipeline(ComputePipelineDesc{ .compute = entry, .label = label });
                if (!created)
                {
                    return std::unexpected(created.error());
                }
                *pipeline = *created;
            }
            return {};
        }

        //==============================================================================================================
        std::expected<void, std::string> createGraphicsPipelines(RojoContext& context, RojoFrameResources& resources)
        {
            const std::array<ShaderEntryName, 2> gbufferEntries{ ShaderEntryName{ "gbufferVertex", "vertex" },
                                                                 ShaderEntryName{ "gbufferFragment", "fragment" } };
            const std::expected<ShaderLibrary, std::string> gbufferLibrary = context.library("BenchGBuffer", gbufferEntries);
            if (!gbufferLibrary)
            {
                return std::unexpected(gbufferLibrary.error());
            }
            const std::array<ShaderEntryName, 1> shadowEntries{ ShaderEntryName{ "shadowVertex", "vertex" } };
            const std::expected<ShaderLibrary, std::string> shadowLibrary = context.library("BenchShadow", shadowEntries);
            if (!shadowLibrary)
            {
                return std::unexpected(shadowLibrary.error());
            }
            const std::array<ShaderEntryName, 2> forwardEntries{ ShaderEntryName{ "forwardVertex", "vertex" },
                                                                 ShaderEntryName{ "forwardFragment", "fragment" } };
            const std::expected<ShaderLibrary, std::string> forwardLibrary = context.library("BenchForward", forwardEntries);
            if (!forwardLibrary)
            {
                return std::unexpected(forwardLibrary.error());
            }
            const std::array<ShaderEntryName, 4> postEntries{ ShaderEntryName{ "fullscreenVertex", "vertex" },
                                                              ShaderEntryName{ "bloomDownFragment", "fragment" },
                                                              ShaderEntryName{ "bloomUpFragment", "fragment" },
                                                              ShaderEntryName{ "toneMapFragment", "fragment" } };
            const std::expected<ShaderLibrary, std::string> postLibrary = context.library("BenchPost", postEntries);
            if (!postLibrary)
            {
                return std::unexpected(postLibrary.error());
            }

            const ShaderEntry fullscreen{ *postLibrary, "fullscreenVertex" };
            const struct
            {
                uint32_t pass;
                ShaderEntry vertex;
                ShaderEntry fragment;
                std::string_view label;
                Pipeline* pipeline;
            } pipelines[] = {
                { kGeometryPass, { *gbufferLibrary, "gbufferVertex" }, { *gbufferLibrary, "gbufferFragment" }, "frame.geometry", &resources.geometry },
                { kShadow0Pass, { *shadowLibrary, "shadowVertex" }, {}, "frame.shadow", &resources.shadow },
                { kForwardPass, { *forwardLibrary, "forwardVertex" }, { *forwardLibrary, "forwardFragment" }, "frame.forward", &resources.forward },
                { kBloomDown0Pass, fullscreen, { *postLibrary, "bloomDownFragment" }, "frame.bloomDown", &resources.bloomDown },
                { kBloomUp1Pass, fullscreen, { *postLibrary, "bloomUpFragment" }, "frame.bloomUp", &resources.bloomUp },
                { kToneMapPass, fullscreen, { *postLibrary, "toneMapFragment" }, "frame.toneMap", &resources.toneMap },
            };
            for (const auto& [pass, vertex, fragment, label, pipeline] : pipelines)
            {
                const std::expected<Pipeline, std::string> created = graphicsPipeline(context, pass, vertex, fragment, label);
                if (!created)
                {
                    return std::unexpected(created.error());
                }
                *pipeline = *created;
            }
            return {};
        }
    } // namespace

    //==================================================================================================================
    Texture RojoFrameResources::texture(FrameTexture which) const
    {
        return textures[static_cast<size_t>(which)];
    }

    //==================================================================================================================
    uint32_t RojoFrameResources::view(FrameTexture which) const
    {
        return static_cast<uint32_t>(sampled[static_cast<size_t>(which)]);
    }

    //==================================================================================================================
    const RenderPassSpec& renderPassSpec(uint32_t pass)
    {
        for (const RenderPassSpec& spec : kRenderPasses)
        {
            if (spec.pass == pass)
            {
                return spec;
            }
        }
        std::fprintf(stderr, "frame: pass %u of kPasses has no render pass\n", pass);
        std::abort();
    }

    //==================================================================================================================
    std::expected<RojoFrameResources, std::string> createFrameResources(RojoContext& context)
    {
        RojoFrameResources resources;
        if (const std::expected<void, std::string> buffers = createBuffers(context, resources); !buffers)
        {
            return std::unexpected(buffers.error());
        }
        if (const std::expected<void, std::string> textures = createTextures(context, resources); !textures)
        {
            return std::unexpected(textures.error());
        }
        if (const std::expected<void, std::string> compute = createComputePipelines(context, resources); !compute)
        {
            return std::unexpected(compute.error());
        }
        if (const std::expected<void, std::string> graphics = createGraphicsPipelines(context, resources); !graphics)
        {
            return std::unexpected(graphics.error());
        }
        return resources;
    }
} // namespace rojoRHI::bench
