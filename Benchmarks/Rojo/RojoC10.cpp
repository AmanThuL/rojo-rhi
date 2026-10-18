//----------------------------------------------------------------------------------------------------------------------
/// @file RojoC10.cpp
/// @brief Implements C10's RojoRHI arm: K counted drawIndirect calls in one render pass over a 256 by 256 R32Float target.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CountedArms.h"
#include "Common/CountedData.h"
#include "Common/Runner.h"
#include "Host/HostClock.h"
#include "Rojo/RojoContext.h"

#include <array>
#include <cstring>
#include <format>

namespace rojoRHI::bench {
    namespace {
        constexpr uint64_t kReadbackTimeoutNs = 60'000'000'000ull;

        class RojoC10
        {
          public:
            using Context = RojoContext;
            static constexpr Arm kArm = Arm::Rojo;

            RojoC10(RojoContext&, const Config&);
            const std::expected<void, std::string>& ready() const;
            std::expected<FrameResult, std::string> frame(uint32_t);
            std::expected<std::vector<Output>, std::string> readback();

          private:
            std::expected<void, std::string> create();

            RojoContext& m_context;
            uint32_t m_k = 0;
            uint32_t m_maxCount = 0;
            bool m_nullCount = false;
            Texture m_target;
            Pipeline m_pipeline;
            Addr m_records{};
            Addr m_count{};
            std::expected<void, std::string> m_ready;
        };

        //==============================================================================================================
        RojoC10::RojoC10(RojoContext& context, const Config& config)
            : m_context(context), m_k(config.k.value_or(0)), m_maxCount(config.maxCount.value_or(kCountedMaxCount)),
              m_nullCount(config.nullCount.value_or(false)), m_ready(create())
        {
        }

        //==============================================================================================================
        // C10m1 draws its one record by the null count; C10 reads maxCount records under a count buffer holding maxCount.
        std::expected<void, std::string> RojoC10::create()
        {
            const std::array<ShaderEntryName, 2> entries{ ShaderEntryName{ "pointVertex", "vertex" }, ShaderEntryName{ "pointFragment", "fragment" } };
            const std::expected<ShaderLibrary, std::string> library = m_context.library("BenchPoints", entries);
            if (!library)
            {
                return std::unexpected(library.error());
            }
            const std::array<ColorTarget, 1> colors{ ColorTarget{ .format = Format::R32Float } };
            const std::expected<Pipeline, std::string> pipeline = m_context.pipeline(
                GraphicsPipelineDesc{
                    .vertex = { *library, "pointVertex" },
                    .fragment = { *library, "pointFragment" },
                    .primitive = PrimitiveClass::Point,
                    .colors = colors,
                    .label = "bench.c10.pipeline",
                }
            );
            if (!pipeline)
            {
                return std::unexpected(pipeline.error());
            }
            m_pipeline = *pipeline;

            const std::expected<Texture, std::string> target = m_context.texture(
                TextureDesc{
                    .format = Format::R32Float,
                    .extent = { kCountedTargetEdge, kCountedTargetEdge, 1 },
                    .usage = TextureUsage::RenderTarget,
                    .label = "bench.c10.target",
                }
            );
            if (!target)
            {
                return std::unexpected(target.error());
            }
            m_target = *target;

            std::vector<std::byte> records = drawRecords(m_maxCount);
            if (m_nullCount)
            {
                const DrawIndirectArgs single{ kSingleDrawId, 1, 1, 0, 1000 };
                std::memcpy(records.data(), &single, sizeof(single));
                records.resize(sizeof(single));
            }
            const std::expected<Buffer, std::string> recordBuffer = m_context.buffer(
                BufferDesc{ .size = records.size(), .memory = Memory::Gpu, .usage = BufferUsage::IndirectArgs, .label = "bench.c10.records" }, records
            );
            if (!recordBuffer)
            {
                return std::unexpected(recordBuffer.error());
            }
            m_records = m_context.device().address(*recordBuffer);

            const std::array<uint32_t, 1> countValue{ m_maxCount };
            const std::expected<Buffer, std::string> countBuffer = m_context.buffer(
                BufferDesc{ .size = sizeof(uint32_t), .memory = Memory::Gpu, .usage = BufferUsage::IndirectArgs, .label = "bench.c10.count" },
                std::as_bytes(std::span(countValue))
            );
            if (!countBuffer)
            {
                return std::unexpected(countBuffer.error());
            }
            m_count = m_context.device().address(*countBuffer);
            return {};
        }

        //==============================================================================================================
        const std::expected<void, std::string>& RojoC10::ready() const
        {
            return m_ready;
        }

        //==============================================================================================================
        std::expected<FrameResult, std::string> RojoC10::frame(uint32_t)
        {
            // The target is cleared by the pass, so each frame discards it into ColorAttachment.
            const std::array<TextureBarrier, 1> toTarget{ TextureBarrier{ .texture = m_target,
                                                                          .srcStage = Stage::All,
                                                                          .dstStage = Stage::All,
                                                                          .dstAccess = Access::ColorAttachmentRead | Access::ColorAttachmentWrite,
                                                                          .oldLayout = Layout::Undefined,
                                                                          .newLayout = Layout::ColorAttachment } };
            const std::array<ColorAttachment, 1> colors{ ColorAttachment{ .texture = m_target, .load = LoadOp::Clear, .store = StoreOp::Store } };
            const RenderPassDesc pass{ .colors = colors, .area = { 0, 0, kCountedTargetEdge, kCountedTargetEdge }, .label = "bench.c10" };
            const Viewport viewport{ .width = static_cast<float>(kCountedTargetEdge), .height = static_cast<float>(kCountedTargetEdge) };
            const Addr count = m_nullCount ? Addr{} : m_count;

            m_context.beginFrame(1);
            const uint64_t start = hostTicks();
            List list = acquire(m_context.device(), QueueType::Graphics, "bench.c10");
            list.barrier({}, {}, toTarget);
            list.beginRenderPass(pass);
            list.setPipeline(m_pipeline);
            list.setViewport(viewport);
            list.setScissor({ 0, 0, kCountedTargetEdge, kCountedTargetEdge });
            for (uint32_t draw = 0; draw < m_k; ++draw)
            {
                list.drawIndirect(m_records, m_maxCount, count);
            }
            list.endRenderPass();
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
        // One frame of its own, outside every measurement: copies the target into a Readback buffer and waits on a fence.
        std::expected<std::vector<Output>, std::string> RojoC10::readback()
        {
            const std::expected<Fence, std::string> fence = m_context.fence(0, "bench.c10.readback");
            if (!fence)
            {
                return std::unexpected(fence.error());
            }
            const std::expected<Buffer, std::string> destination =
                m_context.buffer(BufferDesc{ .size = kCountedTargetBytes, .memory = Memory::Readback, .label = "bench.c10.readback" });
            if (!destination)
            {
                return std::unexpected(destination.error());
            }
            const std::array<TextureBarrier, 1> toCopy{ TextureBarrier{ .texture = m_target,
                                                                        .srcStage = Stage::ColorAttachment,
                                                                        .srcAccess = Access::ColorAttachmentWrite,
                                                                        .dstStage = Stage::Copy,
                                                                        .dstAccess = Access::CopyRead,
                                                                        .oldLayout = Layout::ColorAttachment,
                                                                        .newLayout = Layout::CopySrc } };
            m_context.beginFrame(1);
            List list = acquire(m_context.device(), QueueType::Graphics, "bench.c10.readback");
            list.barrier({}, {}, toCopy);
            list.copyTextureToBuffer(
                m_target,
                TextureRegion{ .extent = { kCountedTargetEdge, kCountedTargetEdge, 1 } },
                *destination,
                BufferTextureLayout{ .rowPitch = kCountedTargetEdge * sizeof(float) }
            );
            list.end();
            const std::array<List, 1> lists{ list };
            const std::array<FenceValue, 1> signal{ FenceValue{ *fence, 1 } };
            m_context.queue(QueueType::Graphics).submit(lists, {}, signal);
            const std::expected<std::optional<double>, std::string> ended = m_context.endFrame();
            if (!ended)
            {
                return std::unexpected("readback: " + ended.error());
            }
            if (!m_context.device().wait(signal[0], kReadbackTimeoutNs))
            {
                return std::unexpected(std::string("readback: the fence did not signal within 60 s"));
            }
            const std::byte* bytes = m_context.device().cpuPointer(*destination);
            std::vector<Output> outputs;
            outputs.push_back(Output{ "target", std::vector<std::byte>(bytes, bytes + kCountedTargetBytes) });
            return outputs;
        }
    } // namespace

    //==================================================================================================================
    ArmResult runRojoC10(const Config& config, const RunOptions& options)
    {
        return runArm<RojoC10>(config, options);
    }
} // namespace rojoRHI::bench
