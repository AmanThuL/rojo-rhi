//----------------------------------------------------------------------------------------------------------------------
/// @file NativeG2.cpp
/// @brief Implements G2's native arms: arm T reads 128 textures through ID tables, arm F from argument-table texture slots.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/KernelArms.h"
#include "Common/KernelPatterns.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "Native/NativeContext.h"

#include <string_view>

namespace rojoRHI::bench {
    namespace {
        constexpr uint64_t kOutputBytes = uint64_t{ kKernelThreads } * sizeof(float);
        constexpr uint64_t kTextureBytes = uint64_t{ kTextureSize } * kTextureSize * sizeof(float);

        // The root the kernels read at buffer 0; BenchSamplesTable.metal and BenchSamplesFixed.metal declare the same layout.
        struct SamplesRoot
        {
            uint64_t output;
            uint32_t viewBase;
            uint32_t samplerIndex;
            uint32_t divergent;
            uint32_t pad0;
            uint32_t pad1;
            uint32_t pad2;
        };

        // How the kernel reaches texture i: through the ID tables at buffers 1 and 2, or from the texture slots.
        enum class TextureAccess : uint8_t
        {
            Table,
            Slots,
        };

        // The setup and frame both arms share; only the kernel, the argument table's contents and the arm name differ.
        class NativeSamples
        {
          public:
            NativeSamples(NativeContext&, const Config&, TextureAccess);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();
            std::expected<void, std::string> upload(MTL::Buffer*, const std::vector<MTL::Texture*>&);

            NativeContext& m_context;
            TextureAccess m_access;
            bool m_divergent = false;
            MTL::Buffer* m_output = nullptr;
            MTL::ComputePipelineState* m_pipeline = nullptr;
            MTL4::ArgumentTable* m_table = nullptr;
            std::expected<void, std::string> m_ready;
        };

        class NativeG2 : public NativeSamples
        {
          public:
            using Context = NativeContext;
            static constexpr Arm kArm = Arm::Native;

            NativeG2(NativeContext&, const Config&);
        };

        class FixedG2 : public NativeSamples
        {
          public:
            using Context = NativeContext;
            static constexpr Arm kArm = Arm::Fixed;

            FixedG2(NativeContext&, const Config&);
        };

        //==============================================================================================================
        NativeSamples::NativeSamples(NativeContext& context, const Config& config, TextureAccess access)
            : m_context(context), m_access(access), m_divergent(config.pattern.value_or("") == "divergent"), m_ready(create())
        {
        }

        //==============================================================================================================
        NativeG2::NativeG2(NativeContext& context, const Config& config) : NativeSamples(context, config, TextureAccess::Table)
        {
        }

        //==============================================================================================================
        FixedG2::FixedG2(NativeContext& context, const Config& config) : NativeSamples(context, config, TextureAccess::Slots)
        {
        }

        //==============================================================================================================
        std::expected<void, std::string> NativeSamples::create()
        {
            std::vector<float> texels;
            texels.reserve(size_t{ kTextureCount } * kTextureSize * kTextureSize);
            for (uint32_t t = 0; t < kTextureCount; ++t)
            {
                const std::vector<float> one = textureTexels(t);
                texels.insert(texels.end(), one.begin(), one.end());
            }
            const std::expected<MTL::Buffer*, std::string> staging =
                m_context.sharedBuffer(texels.size() * sizeof(float), "bench.g2.staging", std::as_bytes(std::span(texels)));
            if (!staging)
            {
                return std::unexpected(staging.error());
            }

            MTL::TextureDescriptor* descriptor = MTL::TextureDescriptor::alloc()->init();
            descriptor->setTextureType(MTL::TextureType2D);
            descriptor->setPixelFormat(MTL::PixelFormatR32Float);
            descriptor->setWidth(kTextureSize);
            descriptor->setHeight(kTextureSize);
            descriptor->setStorageMode(MTL::StorageModePrivate);
            descriptor->setUsage(MTL::TextureUsageShaderRead);
            std::vector<MTL::Texture*> textures;
            textures.reserve(kTextureCount);
            for (uint32_t t = 0; t < kTextureCount; ++t)
            {
                const std::expected<MTL::Texture*, std::string> texture = m_context.texture(descriptor, "bench.g2.texture");
                if (!texture)
                {
                    descriptor->release();
                    return std::unexpected(texture.error());
                }
                textures.push_back(*texture);
            }
            descriptor->release();

            const std::expected<MTL::Buffer*, std::string> output = m_context.privateBuffer(kOutputBytes, "bench.g2.output");
            if (!output)
            {
                return std::unexpected(output.error());
            }
            m_output = *output;
            const SamplesRoot rootValue{
                .output = m_output->gpuAddress(), .viewBase = 0, .samplerIndex = 0, .divergent = m_divergent ? 1u : 0u, .pad0 = 0, .pad1 = 0, .pad2 = 0
            };
            const std::expected<MTL::Buffer*, std::string> root =
                m_context.sharedBuffer(sizeof(SamplesRoot), "bench.g2.root", std::as_bytes(std::span(&rootValue, 1)));
            if (!root)
            {
                return std::unexpected(root.error());
            }

            const bool table = m_access == TextureAccess::Table;
            const std::expected<MTL::Library*, std::string> library = m_context.library(table ? "BenchSamplesTable" : "BenchSamplesFixed");
            if (!library)
            {
                return std::unexpected(library.error());
            }
            const std::expected<MTL::ComputePipelineState*, std::string> pipeline =
                m_context.computePipeline(*library, table ? "samplesTable" : "samplesFixed", table ? "bench.g2.table" : "bench.g2.fixed");
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            m_pipeline = *pipeline;

            MTL4::ArgumentTableDescriptor* tableDescriptor = MTL4::ArgumentTableDescriptor::alloc()->init();
            tableDescriptor->setInitializeBindings(true);
            tableDescriptor->setMaxBufferBindCount(table ? 3 : 1);
            tableDescriptor->setMaxTextureBindCount(table ? 0 : kTextureCount);
            tableDescriptor->setLabel(m_context.string(table ? "bench.g2.arguments.table" : "bench.g2.arguments.fixed"));
            NS::Error* error = nullptr;
            m_table = m_context.device()->newArgumentTable(tableDescriptor, &error);
            tableDescriptor->release();
            ++m_context.calls().host;
            if (m_table == nullptr)
            {
                return std::unexpected("G2: argument table could not be created");
            }
            m_context.own(m_table);
            m_table->setAddress((*root)->gpuAddress(), 0);
            ++m_context.calls().host;

            if (table)
            {
                const std::expected<MTL::Buffer*, std::string> viewIds = m_context.sharedBuffer(kTextureCount * sizeof(MTL::ResourceID), "bench.g2.viewIds");
                if (!viewIds)
                {
                    return std::unexpected(viewIds.error());
                }
                auto* ids = static_cast<MTL::ResourceID*>((*viewIds)->contents());
                for (uint32_t t = 0; t < kTextureCount; ++t)
                {
                    ids[t] = textures[t]->gpuResourceID();
                }

                MTL::SamplerDescriptor* samplerDescriptor = MTL::SamplerDescriptor::alloc()->init();
                samplerDescriptor->setMinFilter(MTL::SamplerMinMagFilterNearest);
                samplerDescriptor->setMagFilter(MTL::SamplerMinMagFilterNearest);
                samplerDescriptor->setMipFilter(MTL::SamplerMipFilterNotMipmapped);
                samplerDescriptor->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
                samplerDescriptor->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
                samplerDescriptor->setNormalizedCoordinates(true);
                samplerDescriptor->setSupportArgumentBuffers(true);
                samplerDescriptor->setLabel(m_context.string("bench.g2.sampler"));
                MTL::SamplerState* sampler = m_context.device()->newSamplerState(samplerDescriptor);
                samplerDescriptor->release();
                ++m_context.calls().host;
                if (sampler == nullptr)
                {
                    return std::unexpected("G2: sampler state could not be created");
                }
                m_context.own(sampler);
                const std::expected<MTL::Buffer*, std::string> samplerIds = m_context.sharedBuffer(sizeof(MTL::ResourceID), "bench.g2.samplerIds");
                if (!samplerIds)
                {
                    return std::unexpected(samplerIds.error());
                }
                *static_cast<MTL::ResourceID*>((*samplerIds)->contents()) = sampler->gpuResourceID();
                m_table->setAddress((*viewIds)->gpuAddress(), 1);
                m_table->setAddress((*samplerIds)->gpuAddress(), 2);
                m_context.calls().host += 2;
            }
            else
            {
                for (uint32_t t = 0; t < kTextureCount; ++t)
                {
                    m_table->setTexture(textures[t]->gpuResourceID(), t);
                    ++m_context.calls().host;
                }
            }

            if (const std::expected<void, std::string> prepared = m_context.prepareFrames(NativeFrameShape{ .lists = 1 }); !prepared)
            {
                return prepared;
            }
            return upload(*staging, textures);
        }

        //==============================================================================================================
        // The texels reach the Private textures through one command buffer, committed and waited before any frame.
        std::expected<void, std::string> NativeSamples::upload(MTL::Buffer* staging, const std::vector<MTL::Texture*>& textures)
        {
            NativeContext::Slot& slot = m_context.beginFrame(0, 1);
            MTL4::CommandBuffer* commands = slot.lists[0];
            commands->beginCommandBuffer(slot.allocators[0]);
            MTL4::ComputeCommandEncoder* encoder = commands->computeCommandEncoder();
            for (uint32_t t = 0; t < kTextureCount; ++t)
            {
                encoder->copyFromBuffer(
                    staging,
                    uint64_t{ t } * kTextureBytes,
                    kTextureSize * sizeof(float),
                    kTextureBytes,
                    MTL::Size::Make(kTextureSize, kTextureSize, 1),
                    textures[t],
                    0,
                    0,
                    MTL::Origin::Make(0, 0, 0)
                );
            }
            encoder->endEncoding();
            commands->endCommandBuffer();
            MTL4::CommandBuffer* const buffers[] = { commands };
            m_context.commit(m_context.graphicsQueue(), buffers);
            const std::expected<std::optional<double>, std::string> done = m_context.endFrame();
            if (!done)
            {
                return std::unexpected("G2: texture upload: " + done.error());
            }
            return {};
        }

        //==============================================================================================================
        const std::expected<void, std::string>& NativeSamples::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        std::expected<FrameResult, std::string> NativeSamples::frame(uint32_t index)
        {
            NativeContext::Slot& slot = m_context.beginFrame(index, 1);
            NativeCalls& calls = m_context.calls();
            MTL4::CommandBuffer* commands = slot.lists[0];

            const uint64_t start = hostTicks();
            commands->beginCommandBuffer(slot.allocators[0]);
            MTL4::ComputeCommandEncoder* encoder = commands->computeCommandEncoder();
            encoder->setComputePipelineState(m_pipeline);
            encoder->setArgumentTable(m_table);
            encoder->dispatchThreadgroups(MTL::Size::Make(kKernelThreads / kKernelGroup, 1, 1), MTL::Size::Make(kKernelGroup, 1, 1));
            encoder->endEncoding();
            commands->endCommandBuffer();
            calls.record += 7;
            const uint64_t end = hostTicks();

            MTL4::CommandBuffer* const buffers[] = { commands };
            m_context.commit(m_context.graphicsQueue(), buffers);
            const std::expected<std::optional<double>, std::string> gpu = m_context.endFrame();
            if (!gpu)
            {
                return std::unexpected(gpu.error());
            }
            return FrameResult{ .cpuNs = ticksToNanoseconds(end - start), .gpuNs = *gpu };
        }

        //==============================================================================================================
        std::expected<std::vector<Output>, std::string> NativeSamples::readback()
        {
            std::expected<std::vector<std::byte>, std::string> bytes = m_context.readBuffer(m_output, 0, kOutputBytes);
            if (!bytes)
            {
                return std::unexpected(bytes.error());
            }
            std::vector<Output> outputs;
            outputs.push_back(Output{ "output", std::move(*bytes) });
            return outputs;
        }
    } // namespace

    //==================================================================================================================
    ArmResult runNativeG2(const Config& config, const RunOptions& options)
    {
        return runArm<NativeG2>(config, options);
    }

    //==================================================================================================================
    ArmResult runFixedG2(const Config& config, const RunOptions& options)
    {
        return runArm<FixedG2>(config, options);
    }
} // namespace rojoRHI::bench
