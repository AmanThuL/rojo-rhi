//----------------------------------------------------------------------------------------------------------------------
/// @file NativeFrameResources.cpp
/// @brief Implements the native arm's composed-frame resources on D3D12: their creation, views, passes, pipelines and dumps.
//----------------------------------------------------------------------------------------------------------------------
#include "NativeD3D12/NativeFrameResources.h"

#include "Common/Scene.h"
#include "NativeD3D12/NativeFrameBarriers.h"

#include <format>
#include <span>
#include <string_view>
#include <utility>

namespace rojoRHI::bench {
    namespace {
        // Pass indices of kPasses whose render pass fixes a pipeline's state.
        constexpr uint32_t kGeometryPass = 1;
        constexpr uint32_t kShadow0Pass = 3;
        constexpr uint32_t kForwardPass = 7;
        constexpr uint32_t kBloomDown0Pass = 8;
        constexpr uint32_t kBloomUp1Pass = 10;
        constexpr uint32_t kToneMapPass = 12;

        // A dump output that is one layer of one texture.
        struct TextureOutput
        {
            std::string_view name;
            FrameTexture texture;
            uint32_t layer;
        };

        // The texture outputs of kOutputs, in its order; records and readback follow them.
        constexpr std::array<TextureOutput, 13> kTextureOutputs{ {
            { "albedo", FrameTexture::Albedo, 0 },
            { "normal", FrameTexture::Normal, 0 },
            { "material", FrameTexture::Material, 0 },
            { "depth", FrameTexture::Depth, 0 },
            { "aoRaw", FrameTexture::AoRaw, 0 },
            { "ao", FrameTexture::Ao, 0 },
            { "shadow0", FrameTexture::Shadow, 0 },
            { "shadow1", FrameTexture::Shadow, 1 },
            { "shadow2", FrameTexture::Shadow, 2 },
            { "hdr", FrameTexture::Hdr, 0 },
            { "ldr", FrameTexture::Ldr, 0 },
            { "bloom0", FrameTexture::Bloom0, 0 },
            { "bloom1", FrameTexture::Bloom1, 0 },
        } };

        static_assert(kTextureOutputs.size() + 2 == kOutputCount, "the outputs are the textures, then records and readback");

        //==============================================================================================================
        // The format a frame texture's views and targets read and write it in: R32_FLOAT for a sampled depth view.
        DXGI_FORMAT viewFormat(Format format)
        {
            switch (format)
            {
            case Format::RGBA8Unorm: return DXGI_FORMAT_R8G8B8A8_UNORM;
            case Format::RGBA16Float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
            case Format::R8Unorm: return DXGI_FORMAT_R8_UNORM;
            case Format::D32Float: return DXGI_FORMAT_R32_FLOAT;
            default: return DXGI_FORMAT_UNKNOWN;
            }
        }

        //==============================================================================================================
        // A depth texture a shader samples is typeless, so it takes a D32_FLOAT depth-stencil view and an R32_FLOAT view.
        DXGI_FORMAT resourceFormat(const TextureSpec& spec)
        {
            if (spec.format != Format::D32Float)
            {
                return viewFormat(spec.format);
            }
            return (spec.usage & TextureUsage::Sampled) != TextureUsage::None ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_D32_FLOAT;
        }

        //==============================================================================================================
        D3D12_RESOURCE_FLAGS resourceFlags(TextureUsage usage)
        {
            const auto has = [usage](TextureUsage bit) { return (usage & bit) != TextureUsage::None; };
            D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
            if (has(TextureUsage::RenderTarget))
            {
                flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            }
            if (has(TextureUsage::DepthStencil))
            {
                flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            }
            if (has(TextureUsage::Storage))
            {
                flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            }
            return flags;
        }

        //==============================================================================================================
        D3D12_BLEND blendFactor(BlendFactor factor)
        {
            switch (factor)
            {
            case BlendFactor::Zero: return D3D12_BLEND_ZERO;
            case BlendFactor::One: return D3D12_BLEND_ONE;
            case BlendFactor::SrcAlpha: return D3D12_BLEND_SRC_ALPHA;
            case BlendFactor::OneMinusSrcAlpha: return D3D12_BLEND_INV_SRC_ALPHA;
            default: return D3D12_BLEND_ZERO;
            }
        }

        //==============================================================================================================
        D3D12_COMPARISON_FUNC compareFunc(CompareOp compare)
        {
            switch (compare)
            {
            case CompareOp::Never: return D3D12_COMPARISON_FUNC_NEVER;
            case CompareOp::Less: return D3D12_COMPARISON_FUNC_LESS;
            case CompareOp::Equal: return D3D12_COMPARISON_FUNC_EQUAL;
            case CompareOp::LessOrEqual: return D3D12_COMPARISON_FUNC_LESS_EQUAL;
            case CompareOp::Greater: return D3D12_COMPARISON_FUNC_GREATER;
            case CompareOp::NotEqual: return D3D12_COMPARISON_FUNC_NOT_EQUAL;
            case CompareOp::GreaterOrEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
            case CompareOp::Always: return D3D12_COMPARISON_FUNC_ALWAYS;
            }
            return D3D12_COMPARISON_FUNC_ALWAYS;
        }

        //==============================================================================================================
        D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE beginning(LoadOp load)
        {
            switch (load)
            {
            case LoadOp::Load: return D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE;
            case LoadOp::Clear: return D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
            case LoadOp::DontCare: return D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD;
            }
            return D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD;
        }

        //==============================================================================================================
        std::expected<ID3D12PipelineState*, std::string> computePipeline(
            NativeContext& context, std::string_view library, std::string_view entry, std::string_view label
        )
        {
            const std::expected<D3D12_SHADER_BYTECODE, std::string> shader = context.library(library, entry);
            if (!shader)
            {
                return std::unexpected(std::format("{}: {}", entry, shader.error()));
            }
            const std::expected<ID3D12PipelineState*, std::string> made = context.computePipeline(*shader, label);
            if (!made)
            {
                return std::unexpected(std::format("{}: {}", entry, made.error()));
            }
            return *made;
        }

        //==============================================================================================================
        // A graphics pipeline with the attachments, rasterisation, depth state and blend of render pass `pass` of kPasses,
        // its blend on color target 0 only; no pixel entry makes a depth-only pipeline.
        std::expected<ID3D12PipelineState*, std::string> renderPipeline(
            NativeContext& context, uint32_t pass, std::string_view library, std::string_view vertex, std::string_view pixel, std::string_view label
        )
        {
            const uint32_t index = renderPassIndex(pass);
            if (index == kRenderPassCount)
            {
                return std::unexpected(std::format("{}: pass {} is no render pass", label, pass));
            }
            const RenderPassSpec& spec = kRenderPasses[index];
            NativeGraphicsDesc desc;
            const std::expected<D3D12_SHADER_BYTECODE, std::string> vertexShader = context.library(library, vertex);
            if (!vertexShader)
            {
                return std::unexpected(std::format("{}: {}", vertex, vertexShader.error()));
            }
            desc.vertex = *vertexShader;
            if (!pixel.empty())
            {
                const std::expected<D3D12_SHADER_BYTECODE, std::string> pixelShader = context.library(library, pixel);
                if (!pixelShader)
                {
                    return std::unexpected(std::format("{}: {}", pixel, pixelShader.error()));
                }
                desc.pixel = *pixelShader;
            }
            desc.rasterizer.CullMode = spec.raster.cull == CullMode::Back ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
            desc.rasterizer.FrontCounterClockwise = spec.raster.frontFace == FrontFace::CounterClockwise ? TRUE : FALSE;
            desc.depthStencil.DepthEnable = spec.depthStencil.depthTest ? TRUE : FALSE;
            desc.depthStencil.DepthWriteMask = spec.depthStencil.depthWrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
            desc.depthStencil.DepthFunc = compareFunc(spec.depthStencil.depthCompare);
            if (spec.blend.enable)
            {
                D3D12_RENDER_TARGET_BLEND_DESC& target = desc.blend.RenderTarget[0];
                target.BlendEnable = TRUE;
                target.SrcBlend = blendFactor(spec.blend.srcColor);
                target.DestBlend = blendFactor(spec.blend.dstColor);
                target.BlendOp = D3D12_BLEND_OP_ADD;
                target.SrcBlendAlpha = blendFactor(spec.blend.srcAlpha);
                target.DestBlendAlpha = blendFactor(spec.blend.dstAlpha);
                target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            }
            for (uint32_t color = 0; color < spec.colorCount; ++color)
            {
                desc.colorFormats.push_back(viewFormat(textureSpec(spec.colors[color].texture).format));
            }
            desc.depthFormat = spec.hasDepth ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_UNKNOWN;
            const std::expected<ID3D12PipelineState*, std::string> made = context.graphicsPipeline(desc, label);
            if (!made)
            {
                return std::unexpected(std::format("{}: {}", vertex, made.error()));
            }
            return *made;
        }
    } // namespace

    //==================================================================================================================
    // Every blend of the frame adds (kAlphaBlend, kAdditiveBlend), as the pipelines above assume.
    std::expected<NativeFramePipelines, std::string> createFramePipelines(NativeContext& context)
    {
        static_assert(kAlphaBlend.colorOp == BlendOp::Add && kAlphaBlend.alphaOp == BlendOp::Add, "the forward blend adds");
        static_assert(kAdditiveBlend.colorOp == BlendOp::Add && kAdditiveBlend.alphaOp == BlendOp::Add, "the bloom blend adds");
        NativeFramePipelines made;
        const struct
        {
            std::string_view library;
            std::string_view entry;
            std::string_view label;
            ID3D12PipelineState** pipeline;
        } kernels[] = {
            { "BenchCull", "cullMain", "bench.frame.cull", &made.cull },
            { "BenchAo", "aoMain", "bench.frame.ao", &made.ao },
            { "BenchAo", "aoBlurMain", "bench.frame.blur", &made.blur },
            { "BenchLighting", "lightingMain", "bench.frame.lighting", &made.lighting },
        };
        for (const auto& [library, entry, label, pipeline] : kernels)
        {
            const std::expected<ID3D12PipelineState*, std::string> kernel = computePipeline(context, library, entry, label);
            if (!kernel)
            {
                return std::unexpected(kernel.error());
            }
            *pipeline = *kernel;
        }

        const struct
        {
            uint32_t pass;
            std::string_view library;
            std::string_view vertex;
            std::string_view pixel;
            std::string_view label;
            ID3D12PipelineState** pipeline;
        } renders[] = {
            { kGeometryPass, "BenchGBuffer", "gbufferVertex", "gbufferFragment", "bench.frame.geometry", &made.geometry },
            { kShadow0Pass, "BenchShadow", "shadowVertex", "", "bench.frame.shadow", &made.shadow },
            { kForwardPass, "BenchForward", "forwardVertex", "forwardFragment", "bench.frame.forward", &made.forward },
            { kBloomDown0Pass, "BenchPost", "fullscreenVertex", "bloomDownFragment", "bench.frame.bloomDown", &made.bloomDown },
            { kBloomUp1Pass, "BenchPost", "fullscreenVertex", "bloomUpFragment", "bench.frame.bloomUp", &made.bloomUp },
            { kToneMapPass, "BenchPost", "fullscreenVertex", "toneMapFragment", "bench.frame.toneMap", &made.toneMap },
        };
        for (const auto& [pass, library, vertex, pixel, label, pipeline] : renders)
        {
            const std::expected<ID3D12PipelineState*, std::string> render = renderPipeline(context, pass, library, vertex, pixel, label);
            if (!render)
            {
                return std::unexpected(render.error());
            }
            *pipeline = *render;
        }
        return made;
    }

    //==================================================================================================================
    NativeFrameResources::NativeFrameResources(NativeContext& context) : m_context(context), m_indirect(context)
    {
    }

    //==================================================================================================================
    std::expected<void, std::string> NativeFrameResources::create()
    {
        if (const std::expected<void, std::string> textures = createTextures(); !textures)
        {
            return textures;
        }
        if (const std::expected<void, std::string> targets = createTargets(); !targets)
        {
            return targets;
        }
        if (const std::expected<void, std::string> buffers = createBuffers(); !buffers)
        {
            return buffers;
        }
        createPasses();
        std::expected<NativeFramePipelines, std::string> made = createFramePipelines(m_context);
        if (!made)
        {
            return std::unexpected(made.error());
        }
        m_pipelines = *made;
        return m_indirect.create();
    }

    //==================================================================================================================
    // Every texture with its sampled and storage views, then the frame's sampler.
    std::expected<void, std::string> NativeFrameResources::createTextures()
    {
        for (uint32_t index = 0; index < kTextureCount; ++index)
        {
            const TextureSpec& spec = kTextures[index];
            const DXGI_FORMAT typed = viewFormat(spec.format);
            if (typed == DXGI_FORMAT_UNKNOWN)
            {
                return std::unexpected(std::format("texture {}: no DXGI format", spec.label));
            }
            D3D12_RESOURCE_DESC1 desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = spec.extent.width;
            desc.Height = spec.extent.height;
            desc.DepthOrArraySize = static_cast<UINT16>(spec.layers);
            desc.MipLevels = 1;
            desc.Format = resourceFormat(spec);
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            desc.Flags = resourceFlags(spec.usage);
            const std::expected<ID3D12Resource*, std::string> created = m_context.texture(desc, D3D12_BARRIER_LAYOUT_COMMON, spec.label);
            if (!created)
            {
                return std::unexpected(created.error());
            }
            m_textures[index] = *created;

            if ((spec.usage & TextureUsage::Sampled) != TextureUsage::None)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC sampled{};
                sampled.Format = typed;
                sampled.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                if (spec.type == TextureType::Tex2DArray)
                {
                    sampled.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
                    sampled.Texture2DArray.MostDetailedMip = 0;
                    sampled.Texture2DArray.MipLevels = 1;
                    sampled.Texture2DArray.FirstArraySlice = 0;
                    sampled.Texture2DArray.ArraySize = spec.layers;
                    sampled.Texture2DArray.PlaneSlice = 0;
                    sampled.Texture2DArray.ResourceMinLODClamp = 0.0f;
                }
                else
                {
                    sampled.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    sampled.Texture2D.MostDetailedMip = 0;
                    sampled.Texture2D.MipLevels = 1;
                    sampled.Texture2D.PlaneSlice = 0;
                    sampled.Texture2D.ResourceMinLODClamp = 0.0f;
                }
                const std::expected<uint32_t, std::string> made = m_context.shaderResourceView(*created, sampled);
                if (!made)
                {
                    return std::unexpected(std::format("{}: {}", spec.label, made.error()));
                }
                m_views[index] = *made;
            }
            if ((spec.usage & TextureUsage::Storage) != TextureUsage::None)
            {
                D3D12_UNORDERED_ACCESS_VIEW_DESC storage{};
                storage.Format = typed;
                storage.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                storage.Texture2D.MipSlice = 0;
                storage.Texture2D.PlaneSlice = 0;
                const std::expected<uint32_t, std::string> made = m_context.unorderedAccessView(*created, storage);
                if (!made)
                {
                    return std::unexpected(std::format("{}: {}", spec.label, made.error()));
                }
                m_storageViews[index] = *made;
            }
        }

        // kFrameSampler: linear, no mip filtering across the textures' single level, clamped on every axis.
        D3D12_SAMPLER_DESC desc{};
        desc.Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        desc.MipLODBias = 0.0f;
        desc.MaxAnisotropy = kFrameSampler.maxAnisotropy;
        desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        desc.MinLOD = 0.0f;
        desc.MaxLOD = D3D12_FLOAT32_MAX;
        const std::expected<uint32_t, std::string> made = m_context.sampler(desc);
        if (!made)
        {
            return std::unexpected(std::format("{}: {}", kFrameSampler.label, made.error()));
        }
        m_sampler = *made;
        return {};
    }

    //==================================================================================================================
    // A render-target view of every color target at its FrameTexture index, and depth-stencil views of `depth` and of
    // each layer of `shadow`, each in a CPU-only heap.
    std::expected<void, std::string> NativeFrameResources::createTargets()
    {
        ID3D12Device12* device = m_context.device();
        NativeCalls& calls = m_context.calls();
        const std::expected<ID3D12DescriptorHeap*, std::string> colorHeap =
            m_context.descriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kTextureCount, false, "bench.frame.rtv");
        if (!colorHeap)
        {
            return std::unexpected(colorHeap.error());
        }
        const std::expected<ID3D12DescriptorHeap*, std::string> depthHeap =
            m_context.descriptorHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, static_cast<uint32_t>(m_depthTargets.size()), false, "bench.frame.dsv");
        if (!depthHeap)
        {
            return std::unexpected(depthHeap.error());
        }
        const UINT colorStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        const D3D12_CPU_DESCRIPTOR_HANDLE colorStart = (*colorHeap)->GetCPUDescriptorHandleForHeapStart();
        const UINT depthStep = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        const D3D12_CPU_DESCRIPTOR_HANDLE depthStart = (*depthHeap)->GetCPUDescriptorHandleForHeapStart();
        calls.host += 4;

        for (uint32_t index = 0; index < kTextureCount; ++index)
        {
            const TextureSpec& spec = kTextures[index];
            if ((spec.usage & TextureUsage::RenderTarget) == TextureUsage::None)
            {
                continue;
            }
            D3D12_RENDER_TARGET_VIEW_DESC target{};
            target.Format = viewFormat(spec.format);
            target.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            target.Texture2D.MipSlice = 0;
            target.Texture2D.PlaneSlice = 0;
            m_renderTargets[index] = D3D12_CPU_DESCRIPTOR_HANDLE{ colorStart.ptr + SIZE_T{ index } * colorStep };
            device->CreateRenderTargetView(m_textures[index], &target, m_renderTargets[index]);
            ++calls.host;
        }

        for (uint32_t index = 0; index < m_depthTargets.size(); ++index)
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC target{};
            target.Format = DXGI_FORMAT_D32_FLOAT;
            target.Flags = D3D12_DSV_FLAG_NONE;
            ID3D12Resource* resource = nullptr;
            if (index == 0)
            {
                target.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
                target.Texture2D.MipSlice = 0;
                resource = m_textures[static_cast<size_t>(FrameTexture::Depth)];
            }
            else
            {
                target.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
                target.Texture2DArray.MipSlice = 0;
                target.Texture2DArray.FirstArraySlice = index - 1;
                target.Texture2DArray.ArraySize = 1;
                resource = m_textures[static_cast<size_t>(FrameTexture::Shadow)];
            }
            m_depthTargets[index] = D3D12_CPU_DESCRIPTOR_HANDLE{ depthStart.ptr + SIZE_T{ index } * depthStep };
            device->CreateDepthStencilView(resource, &target, m_depthTargets[index]);
            ++calls.host;
        }
        return {};
    }

    //==================================================================================================================
    // The scene's buffers with their initial bytes, then per slot records[s], readback[s] and camera[s].
    std::expected<void, std::string> NativeFrameResources::createBuffers()
    {
        const std::vector<Transform> instanceData = allInstances();
        const std::vector<MeshVertex> vertexData = allVertices();
        const std::vector<uint16_t> indexData = allIndices();

        const std::expected<NativeBuffer, std::string> instanceBuffer =
            m_context.defaultBuffer(kInstancesBytes, kInstancesLabel, std::as_bytes(std::span(instanceData)));
        if (!instanceBuffer)
        {
            return std::unexpected(instanceBuffer.error());
        }
        const std::expected<uint32_t, std::string> instanceViews = m_context.bufferViews(instanceBuffer->resource);
        if (!instanceViews)
        {
            return std::unexpected(instanceViews.error());
        }
        m_instancesView = *instanceViews;

        const std::expected<NativeBuffer, std::string> vertexBuffer =
            m_context.defaultBuffer(kVerticesBytes, kVerticesLabel, std::as_bytes(std::span(vertexData)));
        if (!vertexBuffer)
        {
            return std::unexpected(vertexBuffer.error());
        }
        const std::expected<uint32_t, std::string> vertexViews = m_context.bufferViews(vertexBuffer->resource);
        if (!vertexViews)
        {
            return std::unexpected(vertexViews.error());
        }
        m_verticesView = *vertexViews;

        const std::expected<NativeBuffer, std::string> indexBuffer = m_context.defaultBuffer(kIndicesBytes, kIndicesLabel, std::as_bytes(std::span(indexData)));
        if (!indexBuffer)
        {
            return std::unexpected(indexBuffer.error());
        }
        m_indices = D3D12_INDEX_BUFFER_VIEW{ indexBuffer->address, static_cast<UINT>(kIndicesBytes), DXGI_FORMAT_R16_UINT };

        for (uint32_t slot = 0; slot < kFrameSlots; ++slot)
        {
            const std::expected<NativeBuffer, std::string> recordBuffer = m_context.defaultBuffer(kRecordsBytes, kRecordsLabels[slot]);
            if (!recordBuffer)
            {
                return std::unexpected(recordBuffer.error());
            }
            const std::expected<uint32_t, std::string> recordViews = m_context.bufferViews(recordBuffer->resource);
            if (!recordViews)
            {
                return std::unexpected(recordViews.error());
            }
            m_records[slot] = recordBuffer->resource;
            m_recordsViews[slot] = *recordViews;

            const std::expected<NativeBuffer, std::string> rowBuffer = m_context.readbackBuffer(kReadbackBytes, kReadbackLabels[slot]);
            if (!rowBuffer)
            {
                return std::unexpected(rowBuffer.error());
            }
            m_readback[slot] = rowBuffer->resource;

            const std::expected<NativeBuffer, std::string> cameraBuffer =
                m_context.uploadBuffer(kCameraUploadBytes, std::format("bench.frame.camera.{}", slot));
            if (!cameraBuffer)
            {
                return std::unexpected(cameraBuffer.error());
            }
            const std::expected<uint32_t, std::string> cameraViews = m_context.bufferViews(cameraBuffer->resource);
            if (!cameraViews)
            {
                return std::unexpected(cameraViews.error());
            }
            m_cameraData[slot] = cameraBuffer->data;
            m_cameraViews[slot] = *cameraViews;
        }
        return {};
    }

    //==================================================================================================================
    // Every attachment stores; a cleared color clears in its view's format and a cleared depth in D32_FLOAT. No frame
    // texture has a stencil plane. The scissor and the pass area are the same rectangle.
    void NativeFrameResources::createPasses()
    {
        for (uint32_t index = 0; index < kRenderPassCount; ++index)
        {
            const RenderPassSpec& spec = kRenderPasses[index];
            NativeFramePass& made = m_passes[index];
            made.colorCount = spec.colorCount;
            for (uint32_t color = 0; color < spec.colorCount; ++color)
            {
                const ColorPassAttachment& source = spec.colors[color];
                D3D12_RENDER_PASS_RENDER_TARGET_DESC& target = made.colors[color];
                target.cpuDescriptor = m_renderTargets[static_cast<size_t>(source.texture)];
                target.BeginningAccess.Type = beginning(source.load);
                if (source.load == LoadOp::Clear)
                {
                    D3D12_CLEAR_VALUE& clear = target.BeginningAccess.Clear.ClearValue;
                    clear.Format = viewFormat(textureSpec(source.texture).format);
                    clear.Color[0] = source.clear.r;
                    clear.Color[1] = source.clear.g;
                    clear.Color[2] = source.clear.b;
                    clear.Color[3] = source.clear.a;
                }
                target.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
            }
            made.hasDepth = spec.hasDepth;
            if (spec.hasDepth)
            {
                const uint32_t depthView = spec.depth.texture == FrameTexture::Shadow ? 1 + spec.depth.layer : 0;
                made.depth.cpuDescriptor = m_depthTargets[depthView];
                made.depth.DepthBeginningAccess.Type = beginning(spec.depth.load);
                if (spec.depth.load == LoadOp::Clear)
                {
                    made.depth.DepthBeginningAccess.Clear.ClearValue.Format = DXGI_FORMAT_D32_FLOAT;
                    made.depth.DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth = spec.depth.clear;
                    made.depth.DepthBeginningAccess.Clear.ClearValue.DepthStencil.Stencil = 0;
                }
                made.depth.DepthEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
                made.depth.StencilBeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS;
                made.depth.StencilEndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS;
            }
            const Viewport& area = spec.viewport;
            made.viewport = D3D12_VIEWPORT{ area.x, area.y, area.width, area.height, area.minDepth, area.maxDepth };
            const Rect2D& rect = spec.scissor;
            made.scissor = D3D12_RECT{
                static_cast<LONG>(rect.x), static_cast<LONG>(rect.y), static_cast<LONG>(rect.x + rect.width), static_cast<LONG>(rect.y + rect.height)
            };
        }
    }

    //==================================================================================================================
    ID3D12Resource* NativeFrameResources::texture(FrameTexture which) const
    {
        return m_textures[static_cast<size_t>(which)];
    }

    //==================================================================================================================
    uint32_t NativeFrameResources::view(FrameTexture which) const
    {
        return m_views[static_cast<size_t>(which)];
    }

    //==================================================================================================================
    uint32_t NativeFrameResources::storageView(FrameTexture which) const
    {
        return m_storageViews[static_cast<size_t>(which)];
    }

    //==================================================================================================================
    uint32_t NativeFrameResources::sampler() const
    {
        return m_sampler;
    }

    //==================================================================================================================
    // An Addr on D3D12 is the view-heap index of the buffer's raw SRV, then the byte offset.
    Addr NativeFrameResources::instances() const
    {
        return Addr{ m_instancesView, 0 };
    }

    //==================================================================================================================
    Addr NativeFrameResources::vertices() const
    {
        return Addr{ m_verticesView, 0 };
    }

    //==================================================================================================================
    const D3D12_INDEX_BUFFER_VIEW& NativeFrameResources::indices() const
    {
        return m_indices;
    }

    //==================================================================================================================
    ID3D12Resource* NativeFrameResources::records(uint32_t slot) const
    {
        return m_records[slot];
    }

    //==================================================================================================================
    Addr NativeFrameResources::recordsAddr(uint32_t slot) const
    {
        return Addr{ m_recordsViews[slot], 0 };
    }

    //==================================================================================================================
    ID3D12Resource* NativeFrameResources::readback(uint32_t slot) const
    {
        return m_readback[slot];
    }

    //==================================================================================================================
    Addr NativeFrameResources::camera(uint32_t slot) const
    {
        return Addr{ m_cameraViews[slot], 0 };
    }

    //==================================================================================================================
    std::byte* NativeFrameResources::cameraData(uint32_t slot) const
    {
        return m_cameraData[slot];
    }

    //==================================================================================================================
    const NativeFramePass& NativeFrameResources::pass(uint32_t which) const
    {
        return m_passes[renderPassIndex(which)];
    }

    //==================================================================================================================
    const NativeFramePipelines& NativeFrameResources::pipelines() const
    {
        return m_pipelines;
    }

    //==================================================================================================================
    NativeIndirect& NativeFrameResources::indirect()
    {
        return m_indirect;
    }

    //==================================================================================================================
    // Each texture output is read by its own waited setup submission, which moves its subresource to LAYOUT_COPY_SOURCE
    // and back; records[slot] whole, then packed; readback[slot] mapped.
    std::expected<std::vector<Output>, std::string> NativeFrameResources::readOutputs(uint32_t slot)
    {
        std::vector<Output> outputs;
        outputs.reserve(kOutputCount);
        for (const TextureOutput& output : kTextureOutputs)
        {
            std::expected<std::vector<std::byte>, std::string> bytes =
                m_context.readTexture(texture(output.texture), frameEndLayout(output.texture), output.layer);
            if (!bytes)
            {
                return std::unexpected(std::format("frame output {}: {}", output.name, bytes.error()));
            }
            outputs.push_back(Output{ std::string(output.name), std::move(*bytes) });
        }

        const std::expected<std::vector<std::byte>, std::string> recordBytes = m_context.readBuffer(m_records[slot], 0, kRecordsBytes);
        if (!recordBytes)
        {
            return std::unexpected("frame output records: " + recordBytes.error());
        }
        outputs.push_back(Output{ "records", packRecords(*recordBytes) });

        ID3D12Resource* row = m_readback[slot];
        const D3D12_RANGE read{ 0, static_cast<SIZE_T>(kReadbackBytes) };
        void* mapped = nullptr;
        const HRESULT mappedResult = row->Map(0, &read, &mapped);
        ++m_context.calls().host;
        if (FAILED(mappedResult))
        {
            return std::unexpected(std::format("frame output readback: Map failed with HRESULT 0x{:08X}", static_cast<uint32_t>(mappedResult)));
        }
        const auto* rowBytes = static_cast<const std::byte*>(mapped);
        outputs.push_back(Output{ "readback", std::vector<std::byte>(rowBytes, rowBytes + kReadbackBytes) });
        const D3D12_RANGE written{ 0, 0 };
        row->Unmap(0, &written);
        ++m_context.calls().host;

        for (uint32_t index = 0; index < kOutputCount; ++index)
        {
            if (outputs[index].bytes.size() != kOutputs[index].bytes)
            {
                return std::unexpected(
                    std::format(
                        "frame output {}: {} bytes read, the frame's tight layout holds {}",
                        kOutputs[index].name,
                        outputs[index].bytes.size(),
                        kOutputs[index].bytes
                    )
                );
            }
        }
        return outputs;
    }
} // namespace rojoRHI::bench
