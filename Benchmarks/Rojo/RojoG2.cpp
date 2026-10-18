//----------------------------------------------------------------------------------------------------------------------
/// @file RojoG2.cpp
/// @brief Implements G2's RojoRHI arm: one dispatch of 2^20 threads, each summing 16 point samples of 128 textures by ViewIndex and SamplerIndex.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/KernelArms.h"
#include "Common/KernelPatterns.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "Rojo/RojoContext.h"

#include <array>

namespace rojoRHI::bench {
    namespace {
        // Frames in flight; frame f writes region f mod 3 of the output.
        constexpr uint32_t kRegions = 3;
        constexpr uint64_t kRegionBytes = uint64_t{ kKernelThreads } * sizeof(float);

        // The root BenchSamples.slang declares.
        struct SamplesRoot
        {
            Addr output;
            uint32_t viewBase;
            uint32_t samplerIndex;
            uint32_t divergent;
            uint32_t pad0;
            uint32_t pad1;
            uint32_t pad2;
        };

        class RojoG2
        {
          public:
            using Context = RojoContext;
            static constexpr Arm kArm = Arm::Rojo;

            RojoG2(RojoContext&, const Config&);
            ~RojoG2();
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();

            RojoContext& m_context;
            bool m_divergent = false;
            Buffer m_output;
            Pipeline m_pipeline;
            IndexRange m_views;
            SamplerIndex m_sampler{};
            uint32_t m_lastRegion = 0;
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        RojoG2::RojoG2(RojoContext& context, const Config& config)
            : m_context(context), m_divergent(config.pattern.value_or("") == "divergent"), m_ready(create())
        {
        }

        //==============================================================================================================
        // The slots of the view range are released before the context destroys the textures they name.
        RojoG2::~RojoG2()
        {
            if (m_views.count > 0)
            {
                m_context.device().release(m_views);
            }
        }

        //==============================================================================================================
        // The output holds one region per frame slot, so no two frames write the same bytes: the validation tracker
        // reports two writes to one range with no barrier between them (VU-rojo-sync-0021).
        std::expected<void, std::string> RojoG2::create()
        {
            const std::expected<Buffer, std::string> output =
                m_context.buffer(BufferDesc{ .size = kRegions * kRegionBytes, .memory = Memory::Gpu, .label = "bench.g2.output" });
            if (!output)
            {
                return std::unexpected(output.error());
            }
            m_output = *output;

            Result<IndexRange> views = m_context.device().reserveViews(kTextureCount);
            if (!views)
            {
                return std::unexpected("G2: reserveViews: " + views.error().message);
            }
            m_views = *views;
            for (uint32_t t = 0; t < kTextureCount; ++t)
            {
                const std::vector<float> texels = textureTexels(t);
                const TextureData level{ .bytes = std::as_bytes(std::span(texels)), .bytesPerRow = kTextureSize * sizeof(float) };
                const TextureDesc desc{ .type = TextureType::Tex2D,
                                        .format = Format::R32Float,
                                        .extent = Extent3D{ kTextureSize, kTextureSize, 1 },
                                        .usage = TextureUsage::Sampled,
                                        .label = "bench.g2.texture" };
                const std::expected<Texture, std::string> texture = m_context.texture(desc, std::span(&level, 1));
                if (!texture)
                {
                    return std::unexpected(texture.error());
                }
                m_context.device().writeView(m_views, t, *texture, TextureViewDesc{ .type = TextureType::Tex2D, .usage = TextureUsage::Sampled });
            }
            const std::expected<SamplerIndex, std::string> sampler = m_context.sampler(
                SamplerDesc{
                    .filter = SamplerFilter::Nearest, .mipFilter = SamplerFilter::Nearest, .address = SamplerAddress::ClampToEdge, .label = "bench.g2.sampler" }
            );
            if (!sampler)
            {
                return std::unexpected(sampler.error());
            }
            m_sampler = *sampler;

            const std::array<ShaderEntryName, 1> entries{ ShaderEntryName{ "samplesMain", "compute" } };
            const std::expected<ShaderLibrary, std::string> library = m_context.library("BenchSamples", entries);
            if (!library)
            {
                return std::unexpected(library.error());
            }
            const std::expected<Pipeline, std::string> pipeline =
                m_context.pipeline(ComputePipelineDesc{ .compute = ShaderEntry{ .library = *library, .name = "samplesMain" }, .label = "bench.g2" });
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            m_pipeline = *pipeline;
            return {};
        }

        //==============================================================================================================
        const std::expected<void, std::string>& RojoG2::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        std::expected<FrameResult, std::string> RojoG2::frame(uint32_t index)
        {
            m_lastRegion = index % kRegions;
            const SamplesRoot root{
                .output = m_context.device().address(m_output, m_lastRegion * kRegionBytes),
                .viewBase = m_views.base,
                .samplerIndex = static_cast<uint32_t>(m_sampler),
                .divergent = m_divergent ? 1u : 0u,
                .pad0 = 0,
                .pad1 = 0,
                .pad2 = 0,
            };
            m_context.beginFrame(1);
            const uint64_t start = hostTicks();
            List list = acquire(m_context.device(), QueueType::Graphics, "bench.g2");
            list.setPipeline(m_pipeline);
            list.pushRoot(root);
            list.dispatch(kKernelThreads / kKernelGroup, 1, 1);
            list.end();
            const uint64_t end = hostTicks();
            const std::array<List, 1> lists{ list };
            m_context.queue(QueueType::Graphics).submit(lists);
            const std::expected<std::optional<double>, std::string> gpu = m_context.endFrame();
            if (!gpu)
            {
                return std::unexpected(gpu.error());
            }
            return FrameResult{ .cpuNs = ticksToNanoseconds(end - start), .gpuNs = *gpu };
        }

        //==============================================================================================================
        std::expected<std::vector<Output>, std::string> RojoG2::readback()
        {
            const GlobalBarrier written{
                .srcStage = Stage::ComputeShader, .srcAccess = Access::ShaderWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead
            };
            std::expected<std::vector<std::byte>, std::string> bytes = m_context.readBuffer(m_output, m_lastRegion * kRegionBytes, kRegionBytes, written);
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
    ArmResult runRojoG2(const Config& config, const RunOptions& options)
    {
        return runArm<RojoG2>(config, options);
    }
} // namespace rojoRHI::bench
