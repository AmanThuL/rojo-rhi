//----------------------------------------------------------------------------------------------------------------------
/// @file RojoContext.cpp
/// @brief Implements the RojoRHI arm's device, frame sequence, owned objects and readback.
//----------------------------------------------------------------------------------------------------------------------
#include "Rojo/RojoContext.h"

#include <array>
#include <cstring>
#include <format>

namespace rojoRHI::bench {
    namespace {
        constexpr uint64_t kReadbackTimeoutNs = 60'000'000'000;

        //==============================================================================================================
        template<class T> std::expected<T, std::string> named(Result<T> result, std::string_view what, std::string_view label)
        {
            if (!result)
            {
                return std::unexpected(std::format("{} {}: {}", what, label, result.error().message));
            }
            return *result;
        }
    } // namespace

    //==================================================================================================================
    RojoContext::~RojoContext()
    {
        traceFrame(false);
        if (!m_open)
        {
            return;
        }
        // After a failed wait the GPU may still use any of these objects, and still report commits to the collector;
        // the process is about to report and exit, so both are left behind.
        if (m_abandoned)
        {
            (void)m_collector.release();
            (void)m_timing.release();
            return;
        }
        for (auto owned = m_owned.rbegin(); owned != m_owned.rend(); ++owned)
        {
            switch (owned->kind)
            {
            case Kind::Buffer: m_device.destroy(Buffer{ owned->bits }); break;
            case Kind::Texture: m_device.destroy(Texture{ owned->bits }); break;
            case Kind::View: m_device.destroy(static_cast<ViewIndex>(owned->bits)); break;
            case Kind::Sampler: m_device.destroy(static_cast<SamplerIndex>(owned->bits)); break;
            case Kind::Library: m_device.destroy(ShaderLibrary{ owned->bits }); break;
            case Kind::Pipeline: m_device.destroy(Pipeline{ owned->bits }); break;
            case Kind::Fence: m_device.destroy(Fence{ owned->bits }); break;
            }
        }
        if (m_timed)
        {
            m_timing->detach(m_device);
        }
        destroyDevice(m_device);
    }

    //==================================================================================================================
    std::expected<void, std::string> RojoContext::open(const RunOptions& options)
    {
        const DeviceDesc desc{ .validation = options.validation,
                               .enableCapture = false,
                               .scopeTimings = options.scopeTimings,
                               .framesInFlight = 3,
                               .pipelineCache = {},
                               .label = "bench" };
        Result<Device> device = createDevice(desc);
        if (!device)
        {
            return std::unexpected("createDevice: " + device.error().message);
        }
        m_device = *device;
        m_open = true;
        m_timed = m_timing->attach(m_device);
        return {};
    }

    //==================================================================================================================
    Device RojoContext::device() const
    {
        return m_device;
    }

    //==================================================================================================================
    QueueRef RojoContext::queue(QueueType type) const
    {
        Device device = m_device;
        return QueueRef(device.queue(type));
    }

    //==================================================================================================================
    CallSnapshot RojoContext::callSnapshot() const
    {
        const Counters counters = m_device.counters();
        return CallSnapshot{
            .calls = counters.nativeRecordCalls + counters.nativeSubmitCalls + counters.nativeHostCalls,
            .barriers = counters.nativeBarriers,
            .counters = counters,
        };
    }

    //==================================================================================================================
    void RojoContext::beginFrame(uint32_t commits)
    {
        m_collector->begin(commits);
#if defined(ROJORHI_BENCH_BACKEND_NULL)
        traceCall("beginFrame");
#endif
        m_device.beginFrame();
    }

    //==================================================================================================================
    std::expected<std::optional<double>, std::string> RojoContext::endFrame()
    {
#if defined(ROJORHI_BENCH_BACKEND_NULL)
        traceCall("endFrame");
#endif
        m_device.endFrame();
        if (!m_timed)
        {
            return std::optional<double>{};
        }
        const std::expected<double, std::string> span = m_collector->wait();
        if (!span)
        {
            m_abandoned = true;
            return std::unexpected(span.error());
        }
        if (const std::expected<void, std::string> closed = m_timing->closeFrame(); !closed)
        {
            return std::unexpected(closed.error());
        }
        return *span;
    }

    //==================================================================================================================
    void RojoContext::traceFrame(bool on)
    {
        // Metal 4 builds record through CommandList and Queue, which log nothing, so their trace stays empty.
        if (on && !m_tracing)
        {
            startTrace(1024);
        }
        else if (!on && m_tracing)
        {
            stopTrace();
        }
        m_tracing = on;
    }

    //==================================================================================================================
    std::span<const std::string_view> RojoContext::tracedCalls() const
    {
        return bench::tracedCalls();
    }

    //==================================================================================================================
    std::expected<Buffer, std::string> RojoContext::buffer(const BufferDesc& desc, std::span<const std::byte> initial)
    {
        std::expected<Buffer, std::string> buffer = named(m_device.createBuffer(desc, initial), "buffer", desc.label);
        if (buffer)
        {
            m_owned.push_back(Owned{ Kind::Buffer, buffer->bits });
        }
        return buffer;
    }

    //==================================================================================================================
    std::expected<Texture, std::string> RojoContext::texture(const TextureDesc& desc, std::span<const TextureData> initial)
    {
        std::expected<Texture, std::string> texture = named(m_device.createTexture(desc, initial), "texture", desc.label);
        if (texture)
        {
            m_owned.push_back(Owned{ Kind::Texture, texture->bits });
        }
        return texture;
    }

    //==================================================================================================================
    std::expected<ViewIndex, std::string> RojoContext::view(Texture texture, const TextureViewDesc& desc)
    {
        std::expected<ViewIndex, std::string> view = named(m_device.createView(texture, desc), "view of texture", std::to_string(texture.bits));
        if (view)
        {
            m_owned.push_back(Owned{ Kind::View, static_cast<uint64_t>(*view) });
        }
        return view;
    }

    //==================================================================================================================
    std::expected<SamplerIndex, std::string> RojoContext::sampler(const SamplerDesc& desc)
    {
        std::expected<SamplerIndex, std::string> sampler = named(m_device.createSampler(desc), "sampler", "");
        if (sampler)
        {
            m_owned.push_back(Owned{ Kind::Sampler, static_cast<uint64_t>(*sampler) });
        }
        return sampler;
    }

    //==================================================================================================================
    std::expected<ShaderLibrary, std::string> RojoContext::library(std::string_view name, std::span<const ShaderEntryName> entries)
    {
        std::expected<ShaderLibrary, std::string> library = loadLibrary(m_device, name, entries);
        if (library)
        {
            m_owned.push_back(Owned{ Kind::Library, library->bits });
        }
        return library;
    }

    //==================================================================================================================
    std::expected<Pipeline, std::string> RojoContext::pipeline(const GraphicsPipelineDesc& desc)
    {
        std::expected<Pipeline, std::string> pipeline = named(m_device.createPipeline(desc), "graphics pipeline", desc.label);
        if (pipeline)
        {
            m_owned.push_back(Owned{ Kind::Pipeline, pipeline->bits });
        }
        return pipeline;
    }

    //==================================================================================================================
    std::expected<Pipeline, std::string> RojoContext::pipeline(const ComputePipelineDesc& desc)
    {
        std::expected<Pipeline, std::string> pipeline = named(m_device.createPipeline(desc), "compute pipeline", desc.label);
        if (pipeline)
        {
            m_owned.push_back(Owned{ Kind::Pipeline, pipeline->bits });
        }
        return pipeline;
    }

    //==================================================================================================================
    std::expected<Fence, std::string> RojoContext::fence(uint64_t initial, std::string_view label)
    {
        std::expected<Fence, std::string> fence = named(m_device.createFence(initial, label), "fence", label);
        if (fence)
        {
            m_owned.push_back(Owned{ Kind::Fence, fence->bits });
        }
        return fence;
    }

    //==================================================================================================================
    std::expected<std::vector<std::byte>, std::string> RojoContext::readBuffer(Buffer source, uint64_t offset, uint64_t size, const GlobalBarrier& before)
    {
        if (m_readbackFence == Fence{})
        {
            const std::expected<Fence, std::string> created = fence(0, "bench.readback");
            if (!created)
            {
                return std::unexpected(created.error());
            }
            m_readbackFence = *created;
        }
        const std::expected<Buffer, std::string> target = buffer(BufferDesc{ .size = size, .memory = Memory::Readback, .label = "bench.readback" });
        if (!target)
        {
            return std::unexpected(target.error());
        }

        beginFrame(1);
        List list = acquire(m_device, QueueType::Graphics, "bench.readback");
        const std::array<GlobalBarrier, 1> globals{ before };
        list.barrier(globals);
        list.copyBuffer(source, offset, *target, 0, size);
        list.end();
        const std::array<List, 1> lists{ list };
        const std::array<FenceValue, 1> signal{ FenceValue{ m_readbackFence, ++m_readbackValue } };
        queue(QueueType::Graphics).submit(lists, {}, signal);
        const std::expected<std::optional<double>, std::string> ended = endFrame();
        if (!ended)
        {
            return std::unexpected("readback: " + ended.error());
        }
        if (!m_device.wait(signal[0], kReadbackTimeoutNs))
        {
            m_abandoned = true;
            return std::unexpected(std::string("readback: the fence did not signal within 60 s"));
        }
        const std::byte* bytes = m_device.cpuPointer(*target);
        return std::vector<std::byte>(bytes, bytes + size);
    }
} // namespace rojoRHI::bench
