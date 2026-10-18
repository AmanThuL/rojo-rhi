//----------------------------------------------------------------------------------------------------------------------
/// @file NativeFrameResources.cpp
/// @brief Implements the native arm's composed-frame resources, their creation, residency and dumps.
//----------------------------------------------------------------------------------------------------------------------
#include "Native/NativeFrameResources.h"

#include "Common/Scene.h"

#include <format>
#include <span>
#include <string_view>

namespace rojoRHI::bench {
    namespace {
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

        constexpr uint64_t kDumpAlignment = 256;

        //==============================================================================================================
        // Bytes of one layer of a texture in its dump, rows dumpRowPitch apart.
        constexpr uint64_t paddedLayerBytes(FrameTexture texture)
        {
            return uint64_t{ dumpRowPitch(texture) } * textureSpec(texture).extent.height;
        }

        //==============================================================================================================
        // The offset of each texture output in the dump buffer, then of records[s], then the buffer's size.
        constexpr std::array<uint64_t, kTextureOutputs.size() + 2> dumpOffsets()
        {
            std::array<uint64_t, kTextureOutputs.size() + 2> offsets{};
            uint64_t offset = 0;
            for (size_t index = 0; index < kTextureOutputs.size(); ++index)
            {
                offsets[index] = offset;
                offset += (paddedLayerBytes(kTextureOutputs[index].texture) + kDumpAlignment - 1) / kDumpAlignment * kDumpAlignment;
            }
            offsets[kTextureOutputs.size()] = offset;
            offsets[kTextureOutputs.size() + 1] = offset + kRecordsBytes;
            return offsets;
        }

        constexpr std::array<uint64_t, kTextureOutputs.size() + 2> kDumpOffsets = dumpOffsets();
        constexpr uint64_t kDumpRecordsOffset = kDumpOffsets[kTextureOutputs.size()];
        constexpr uint64_t kDumpBytes = kDumpOffsets[kTextureOutputs.size() + 1];

        //==============================================================================================================
        MTL::PixelFormat pixelFormat(Format format)
        {
            switch (format)
            {
            case Format::RGBA8Unorm: return MTL::PixelFormatRGBA8Unorm;
            case Format::RGBA16Float: return MTL::PixelFormatRGBA16Float;
            case Format::R8Unorm: return MTL::PixelFormatR8Unorm;
            case Format::D32Float: return MTL::PixelFormatDepth32Float;
            default: return MTL::PixelFormatInvalid;
            }
        }

        //==============================================================================================================
        // Sampled reads, Storage reads and writes, and attachments of either kind map to Metal's three usages.
        MTL::TextureUsage textureUsage(TextureUsage usage)
        {
            const auto has = [usage](TextureUsage bit) { return (usage & bit) != TextureUsage::None; };
            MTL::TextureUsage result = MTL::TextureUsageUnknown;
            if (has(TextureUsage::Sampled) || has(TextureUsage::Storage))
            {
                result |= MTL::TextureUsageShaderRead;
            }
            if (has(TextureUsage::Storage))
            {
                result |= MTL::TextureUsageShaderWrite;
            }
            if (has(TextureUsage::RenderTarget) || has(TextureUsage::DepthStencil))
            {
                result |= MTL::TextureUsageRenderTarget;
            }
            return result;
        }

        //==============================================================================================================
        template<class T> std::span<const std::byte> bytesOf(const std::vector<T>& values)
        {
            return std::as_bytes(std::span(values));
        }
    } // namespace

    //==================================================================================================================
    NativeFrameResources::NativeFrameResources(NativeContext& context) : m_context(context)
    {
    }

    //==================================================================================================================
    std::expected<void, std::string> NativeFrameResources::create()
    {
        if (const std::expected<void, std::string> textures = createTextures(); !textures)
        {
            return textures;
        }
        if (const std::expected<void, std::string> buffers = createBuffers(); !buffers)
        {
            return buffers;
        }
        return createTables();
    }

    //==================================================================================================================
    std::expected<void, std::string> NativeFrameResources::createTextures()
    {
        for (uint32_t index = 0; index < kTextureCount; ++index)
        {
            const TextureSpec& spec = kTextures[index];
            const MTL::PixelFormat format = pixelFormat(spec.format);
            if (format == MTL::PixelFormatInvalid)
            {
                return std::unexpected(std::format("texture {}: no Metal pixel format", spec.label));
            }
            MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
            descriptor->setTextureType(spec.type == TextureType::Tex2DArray ? MTL::TextureType2DArray : MTL::TextureType2D);
            descriptor->setPixelFormat(format);
            descriptor->setWidth(spec.extent.width);
            descriptor->setHeight(spec.extent.height);
            descriptor->setArrayLength(spec.layers);
            descriptor->setStorageMode(MTL::StorageModePrivate);
            descriptor->setUsage(textureUsage(spec.usage));
            const std::expected<MTL::Texture*, std::string> texture = m_context.texture(descriptor, spec.label);
            descriptor->release();
            if (!texture)
            {
                return std::unexpected(texture.error());
            }
            m_textures[index] = *texture;
        }
        return {};
    }

    //==================================================================================================================
    std::expected<void, std::string> NativeFrameResources::createBuffers()
    {
        const std::expected<MTL::Buffer*, std::string> instances = m_context.privateBuffer(kInstancesBytes, kInstancesLabel, bytesOf(allInstances()));
        if (!instances)
        {
            return std::unexpected(instances.error());
        }
        m_instances = *instances;
        const std::expected<MTL::Buffer*, std::string> vertices = m_context.privateBuffer(kVerticesBytes, kVerticesLabel, bytesOf(allVertices()));
        if (!vertices)
        {
            return std::unexpected(vertices.error());
        }
        m_vertices = *vertices;
        const std::expected<MTL::Buffer*, std::string> indices = m_context.privateBuffer(kIndicesBytes, kIndicesLabel, bytesOf(allIndices()));
        if (!indices)
        {
            return std::unexpected(indices.error());
        }
        m_indices = *indices;

        for (uint32_t slot = 0; slot < kFrameSlots; ++slot)
        {
            const std::expected<MTL::Buffer*, std::string> records = m_context.privateBuffer(kRecordsBytes, kRecordsLabels[slot]);
            if (!records)
            {
                return std::unexpected(records.error());
            }
            m_records[slot] = *records;
            const std::expected<MTL::Buffer*, std::string> readback = m_context.sharedBuffer(kReadbackBytes, kReadbackLabels[slot]);
            if (!readback)
            {
                return std::unexpected(readback.error());
            }
            m_readback[slot] = *readback;
        }

        const std::expected<MTL::Buffer*, std::string> dump = m_context.sharedBuffer(kDumpBytes, "frame.dump");
        if (!dump)
        {
            return std::unexpected(dump.error());
        }
        m_dump = *dump;
        return {};
    }

    //==================================================================================================================
    std::expected<void, std::string> NativeFrameResources::createTables()
    {
        std::vector<MTL::ResourceID> views(kTextureCount);
        for (uint32_t index = 0; index < kTextureCount; ++index)
        {
            views[index] = m_textures[index]->gpuResourceID();
        }
        const std::expected<MTL::Buffer*, std::string> viewIds =
            m_context.sharedBuffer(views.size() * sizeof(MTL::ResourceID), "frame.viewIds", bytesOf(views));
        if (!viewIds)
        {
            return std::unexpected(viewIds.error());
        }
        m_viewIds = *viewIds;

        // kFrameSampler: linear, no mip filtering across the textures' single level, clamped on every axis.
        MTL::SamplerDescriptor* descriptor = MTL::SamplerDescriptor::alloc()->init();
        descriptor->setMinFilter(MTL::SamplerMinMagFilterLinear);
        descriptor->setMagFilter(MTL::SamplerMinMagFilterLinear);
        descriptor->setMipFilter(MTL::SamplerMipFilterNearest);
        descriptor->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
        descriptor->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
        descriptor->setRAddressMode(MTL::SamplerAddressModeClampToEdge);
        descriptor->setMaxAnisotropy(kFrameSampler.maxAnisotropy);
        descriptor->setNormalizedCoordinates(true);
        descriptor->setSupportArgumentBuffers(true);
        descriptor->setLabel(m_context.string(kFrameSampler.label));
        MTL::SamplerState* sampler = m_context.device()->newSamplerState(descriptor);
        descriptor->release();
        ++m_context.calls().host;
        if (sampler == nullptr)
        {
            return std::unexpected("sampler frame.sampler: the device refused the descriptor");
        }
        m_context.own(sampler);
        const std::vector<MTL::ResourceID> samplers{ sampler->gpuResourceID() };
        const std::expected<MTL::Buffer*, std::string> samplerIds = m_context.sharedBuffer(sizeof(MTL::ResourceID), "frame.samplerIds", bytesOf(samplers));
        if (!samplerIds)
        {
            return std::unexpected(samplerIds.error());
        }
        m_samplerIds = *samplerIds;
        return {};
    }

    //==================================================================================================================
    MTL::Texture* NativeFrameResources::texture(FrameTexture texture) const
    {
        return m_textures[static_cast<size_t>(texture)];
    }

    //==================================================================================================================
    uint32_t NativeFrameResources::view(FrameTexture texture) const
    {
        return static_cast<uint32_t>(texture);
    }

    //==================================================================================================================
    uint32_t NativeFrameResources::sampler() const
    {
        return 0;
    }

    //==================================================================================================================
    uint64_t NativeFrameResources::viewTable() const
    {
        return m_viewIds->gpuAddress();
    }

    //==================================================================================================================
    uint64_t NativeFrameResources::samplerTable() const
    {
        return m_samplerIds->gpuAddress();
    }

    //==================================================================================================================
    MTL::Buffer* NativeFrameResources::instances() const
    {
        return m_instances;
    }

    //==================================================================================================================
    MTL::Buffer* NativeFrameResources::vertices() const
    {
        return m_vertices;
    }

    //==================================================================================================================
    MTL::Buffer* NativeFrameResources::indices() const
    {
        return m_indices;
    }

    //==================================================================================================================
    MTL::Buffer* NativeFrameResources::records(uint32_t slot) const
    {
        return m_records[slot];
    }

    //==================================================================================================================
    MTL::Buffer* NativeFrameResources::readback(uint32_t slot) const
    {
        return m_readback[slot];
    }

    //==================================================================================================================
    std::expected<std::vector<Output>, std::string> NativeFrameResources::readOutputs(uint32_t slot, uint32_t commitIndex)
    {
        NativeContext::Slot& frame = m_context.beginFrame(commitIndex, 1);
        NativeCalls& calls = m_context.calls();
        MTL4::CommandBuffer* commands = frame.lists[0];
        commands->beginCommandBuffer(frame.allocators[0]);
        MTL4::ComputeCommandEncoder* encoder = commands->computeCommandEncoder();
        calls.record += 2;
        for (size_t index = 0; index < kTextureOutputs.size(); ++index)
        {
            const TextureOutput& output = kTextureOutputs[index];
            const TextureSpec& spec = textureSpec(output.texture);
            encoder->copyFromTexture(
                texture(output.texture),
                output.layer,
                0,
                MTL::Origin::Make(0, 0, 0),
                MTL::Size::Make(spec.extent.width, spec.extent.height, 1),
                m_dump,
                kDumpOffsets[index],
                dumpRowPitch(output.texture),
                paddedLayerBytes(output.texture)
            );
            ++calls.record;
        }
        encoder->copyFromBuffer(m_records[slot], 0, m_dump, kDumpRecordsOffset, kRecordsBytes);
        encoder->endEncoding();
        commands->endCommandBuffer();
        calls.record += 3;
        MTL4::CommandBuffer* const buffers[] = { commands };
        m_context.commit(m_context.graphicsQueue(), buffers);
        if (const std::expected<std::optional<double>, std::string> done = m_context.endFrame(); !done)
        {
            return std::unexpected("frame outputs: " + done.error());
        }

        const auto* dump = static_cast<const std::byte*>(m_dump->contents());
        std::vector<Output> outputs;
        outputs.reserve(kOutputCount);
        for (size_t index = 0; index < kTextureOutputs.size(); ++index)
        {
            const TextureOutput& output = kTextureOutputs[index];
            const TextureSpec& spec = textureSpec(output.texture);
            const std::span<const std::byte> padded(dump + kDumpOffsets[index], paddedLayerBytes(output.texture));
            outputs.push_back(
                Output{ std::string(output.name), repackRows(padded, spec.extent.width * spec.bytesPerTexel, dumpRowPitch(output.texture), spec.extent.height) }
            );
        }
        outputs.push_back(Output{ "records", packRecords(std::span(dump + kDumpRecordsOffset, kRecordsBytes)) });
        const auto* row = static_cast<const std::byte*>(m_readback[slot]->contents());
        outputs.push_back(Output{ "readback", std::vector<std::byte>(row, row + kReadbackBytes) });
        return outputs;
    }
} // namespace rojoRHI::bench
