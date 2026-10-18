//----------------------------------------------------------------------------------------------------------------------
/// @file NativeContext.cpp
/// @brief Implements the native arm's Metal 4 setup, frame slots, feedback commits, readback and teardown.
//----------------------------------------------------------------------------------------------------------------------
#include "Native/NativeContext.h"

#include "Host/ExecutableDirectory.h"

#include <algorithm>
#include <filesystem>
#include <format>

// The two calls @autoreleasepool lowers to. A pushed pool allocates nothing on the heap, unlike an NSAutoreleasePool
// object, so a frame's pool adds no allocation to N's count.
extern "C" void* objc_autoreleasePoolPush(void);
extern "C" void objc_autoreleasePoolPop(void*);

namespace rojoRHI::bench {
    namespace {
        // An autorelease pool around one setup function, so the autoreleasing factories it calls leave nothing behind.
        class PoolScope
        {
          public:
            PoolScope();
            ~PoolScope();

          private:
            void* m_pool;
        };

        //==============================================================================================================
        PoolScope::PoolScope() : m_pool(objc_autoreleasePoolPush())
        {
        }

        //==============================================================================================================
        PoolScope::~PoolScope()
        {
            objc_autoreleasePoolPop(m_pool);
        }

        //==============================================================================================================
        // An autoreleased string; valid inside the caller's pool.
        NS::String* temporaryString(std::string_view text)
        {
            return NS::String::string(std::string(text).c_str(), NS::UTF8StringEncoding);
        }
    } // namespace

    //==================================================================================================================
    std::string describe(NS::Error* error)
    {
        if (error == nullptr || error->localizedDescription() == nullptr)
        {
            return "no error description";
        }
        return error->localizedDescription()->utf8String();
    }

    //==================================================================================================================
    NativeContext::~NativeContext()
    {
        if (m_framePool != nullptr)
        {
            objc_autoreleasePoolPop(m_framePool);
        }
        // After a failed wait the GPU may still read any of these objects, and the process is about to report and exit.
        if (m_abandoned)
        {
            (void)m_collector.release();
            return;
        }
        for (auto object = m_owned.rbegin(); object != m_owned.rend(); ++object)
        {
            (*object)->release();
        }
    }

    //==================================================================================================================
    std::expected<void, std::string> NativeContext::open(const RunOptions& /*options*/)
    {
        PoolScope pool;
        m_device = MTL::CreateSystemDefaultDevice();
        if (m_device == nullptr)
        {
            return std::unexpected(std::string("no Metal device"));
        }
        own(m_device);
        if (!m_device->supportsFamily(MTL::GPUFamilyMetal4))
        {
            return std::unexpected(std::format("{} does not support the Metal 4 family", m_device->name()->utf8String()));
        }

        NS::Error* error = nullptr;
        MTL4::CommandQueueDescriptor* queueDescriptor = MTL4::CommandQueueDescriptor::alloc()->init();
        queueDescriptor->setLabel(temporaryString("bench.graphics"));
        m_graphics = m_device->newMTL4CommandQueue(queueDescriptor, &error);
        ++m_calls.host;
        if (m_graphics == nullptr)
        {
            queueDescriptor->release();
            return std::unexpected("graphics queue: " + describe(error));
        }
        own(m_graphics);
        error = nullptr;
        queueDescriptor->setLabel(temporaryString("bench.compute"));
        m_compute = m_device->newMTL4CommandQueue(queueDescriptor, &error);
        ++m_calls.host;
        queueDescriptor->release();
        if (m_compute == nullptr)
        {
            return std::unexpected("compute queue: " + describe(error));
        }
        own(m_compute);

        error = nullptr;
        MTL::ResidencySetDescriptor* residencyDescriptor = MTL::ResidencySetDescriptor::alloc()->init();
        residencyDescriptor->setLabel(temporaryString("bench.residency"));
        m_residency = m_device->newResidencySet(residencyDescriptor, &error);
        ++m_calls.host;
        residencyDescriptor->release();
        if (m_residency == nullptr)
        {
            return std::unexpected("residency set: " + describe(error));
        }
        own(m_residency);
        m_graphics->addResidencySet(m_residency);
        m_compute->addResidencySet(m_residency);
        m_calls.submit += 2;

        error = nullptr;
        MTL4::CompilerDescriptor* compilerDescriptor = MTL4::CompilerDescriptor::alloc()->init();
        m_compiler = m_device->newCompiler(compilerDescriptor, &error);
        ++m_calls.host;
        compilerDescriptor->release();
        if (m_compiler == nullptr)
        {
            return std::unexpected("compiler: " + describe(error));
        }
        own(m_compiler);

        m_setupAllocator = m_device->newCommandAllocator();
        m_setupList = m_device->newCommandBuffer();
        m_calls.host += 2;
        if (m_setupAllocator == nullptr || m_setupList == nullptr)
        {
            return std::unexpected(std::string("setup command allocator or command buffer"));
        }
        own(m_setupAllocator);
        own(m_setupList);
        m_pendingOptions.reserve(FeedbackCollector::kDefaultCapacity);
        return {};
    }

    //==================================================================================================================
    MTL::Device* NativeContext::device() const
    {
        return m_device;
    }

    //==================================================================================================================
    MTL4::CommandQueue* NativeContext::graphicsQueue() const
    {
        return m_graphics;
    }

    //==================================================================================================================
    MTL4::CommandQueue* NativeContext::computeQueue() const
    {
        return m_compute;
    }

    //==================================================================================================================
    MTL4::Compiler* NativeContext::compiler() const
    {
        return m_compiler;
    }

    //==================================================================================================================
    MTL::ResidencySet* NativeContext::residency() const
    {
        return m_residency;
    }

    //==================================================================================================================
    NativeCalls& NativeContext::calls()
    {
        return m_calls;
    }

    //==================================================================================================================
    CallSnapshot NativeContext::callSnapshot() const
    {
        return CallSnapshot{ .calls = m_calls.record + m_calls.submit + m_calls.host, .barriers = m_calls.barriers };
    }

    //==================================================================================================================
    FeedbackCollector& NativeContext::collector()
    {
        return *m_collector;
    }

    //==================================================================================================================
    void NativeContext::own(NS::Object* object)
    {
        m_owned.push_back(object);
    }

    //==================================================================================================================
    NS::String* NativeContext::string(std::string_view text)
    {
        NS::String* value = NS::String::alloc()->init(std::string(text).c_str(), NS::UTF8StringEncoding);
        own(value);
        return value;
    }

    //==================================================================================================================
    std::expected<MTL::Buffer*, std::string> NativeContext::sharedBuffer(uint64_t size, std::string_view label, std::span<const std::byte> initial)
    {
        if (size == 0 || initial.size() > size)
        {
            return std::unexpected(std::format("buffer {}: {} bytes cannot hold {} initial bytes", label, size, initial.size()));
        }
        PoolScope pool;
        MTL::Buffer* buffer = m_device->newBuffer(size, MTL::ResourceStorageModeShared);
        ++m_calls.host;
        if (buffer == nullptr)
        {
            return std::unexpected(std::format("buffer {}: the device refused {} Shared bytes", label, size));
        }
        own(buffer);
        buffer->setLabel(temporaryString(label));
        std::memset(buffer->contents(), 0, size);
        if (!initial.empty())
        {
            std::memcpy(buffer->contents(), initial.data(), initial.size());
        }
        m_residency->addAllocation(buffer);
        ++m_calls.host;
        return buffer;
    }

    //==================================================================================================================
    std::expected<MTL::Buffer*, std::string> NativeContext::privateBuffer(uint64_t size, std::string_view label, std::span<const std::byte> initial)
    {
        if (size == 0 || initial.size() > size)
        {
            return std::unexpected(std::format("buffer {}: {} bytes cannot hold {} initial bytes", label, size, initial.size()));
        }
        PoolScope pool;
        MTL::Buffer* buffer = m_device->newBuffer(size, MTL::ResourceStorageModePrivate);
        ++m_calls.host;
        if (buffer == nullptr)
        {
            return std::unexpected(std::format("buffer {}: the device refused {} Private bytes", label, size));
        }
        own(buffer);
        buffer->setLabel(temporaryString(label));
        m_residency->addAllocation(buffer);
        ++m_calls.host;
        if (!initial.empty())
        {
            const std::expected<MTL::Buffer*, std::string> staging = sharedBuffer(initial.size(), std::string(label) + ".staging", initial);
            if (!staging)
            {
                return std::unexpected(staging.error());
            }
            if (const std::expected<void, std::string> copied = runSetupCommit(*staging, 0, buffer, 0, initial.size()); !copied)
            {
                return std::unexpected(std::format("buffer {}: initial upload: {}", label, copied.error()));
            }
        }
        return buffer;
    }

    //==================================================================================================================
    std::expected<MTL::Texture*, std::string> NativeContext::texture(const MTL::TextureDescriptor* descriptor, std::string_view label)
    {
        PoolScope pool;
        MTL::Texture* texture = m_device->newTexture(descriptor);
        ++m_calls.host;
        if (texture == nullptr)
        {
            return std::unexpected(std::format("texture {}: the device refused the descriptor", label));
        }
        own(texture);
        texture->setLabel(temporaryString(label));
        m_residency->addAllocation(texture);
        ++m_calls.host;
        return texture;
    }

    //==================================================================================================================
    std::expected<MTL::Library*, std::string> NativeContext::library(std::string_view name)
    {
        const std::expected<std::filesystem::path, std::string> directory = executableDirectory();
        if (!directory)
        {
            return std::unexpected(std::format("library {}: {}", name, directory.error()));
        }
        const std::filesystem::path path = *directory / "Shaders" / (std::string(name) + ".metallib");
        if (!std::filesystem::exists(path))
        {
            return std::unexpected(std::format("library {}: {} does not exist; the rojorhi.metal or rojorhi.slang rule builds it", name, path.string()));
        }
        PoolScope pool;
        NS::Error* error = nullptr;
        MTL::Library* library = m_device->newLibrary(NS::URL::fileURLWithPath(temporaryString(path.string())), &error);
        ++m_calls.host;
        if (library == nullptr)
        {
            return std::unexpected(std::format("library {}: {}", path.string(), describe(error)));
        }
        own(library);
        return library;
    }

    //==================================================================================================================
    MTL4::LibraryFunctionDescriptor* NativeContext::function(MTL::Library* library, std::string_view name)
    {
        MTL4::LibraryFunctionDescriptor* descriptor = MTL4::LibraryFunctionDescriptor::alloc()->init();
        own(descriptor);
        descriptor->setLibrary(library);
        descriptor->setName(string(name));
        return descriptor;
    }

    //==================================================================================================================
    std::expected<MTL::ComputePipelineState*, std::string> NativeContext::computePipeline(
        MTL::Library* library, std::string_view function, std::string_view label
    )
    {
        PoolScope pool;
        MTL4::ComputePipelineDescriptor* descriptor = MTL4::ComputePipelineDescriptor::alloc()->init();
        descriptor->setComputeFunctionDescriptor(this->function(library, function));
        descriptor->setLabel(temporaryString(label));
        NS::Error* error = nullptr;
        MTL::ComputePipelineState* pipeline = m_compiler->newComputePipelineState(descriptor, nullptr, &error);
        ++m_calls.host;
        descriptor->release();
        if (pipeline == nullptr)
        {
            return std::unexpected(std::format("compute pipeline {} ({}): {}", label, function, describe(error)));
        }
        own(pipeline);
        return pipeline;
    }

    //==================================================================================================================
    std::expected<MTL::RenderPipelineState*, std::string> NativeContext::renderPipeline(MTL4::RenderPipelineDescriptor* descriptor, std::string_view label)
    {
        PoolScope pool;
        descriptor->setSupportIndirectCommandBuffers(MTL4::IndirectCommandBufferSupportStateEnabled);
        descriptor->setLabel(temporaryString(label));
        NS::Error* error = nullptr;
        MTL::RenderPipelineState* pipeline = m_compiler->newRenderPipelineState(descriptor, nullptr, &error);
        ++m_calls.host;
        if (pipeline == nullptr)
        {
            return std::unexpected(std::format("render pipeline {}: {}", label, describe(error)));
        }
        own(pipeline);
        return pipeline;
    }

    //==================================================================================================================
    std::expected<void, std::string> NativeContext::prepareFrames(const NativeFrameShape& shape)
    {
        if (!m_slots.empty() || shape.lists == 0)
        {
            return std::unexpected(std::format("frame slots: prepared twice or with {} lists", shape.lists));
        }
        PoolScope pool;
        m_slots.resize(kFrameSlots);
        for (uint32_t index = 0; index < kFrameSlots; ++index)
        {
            Slot& slot = m_slots[index];
            for (uint32_t list = 0; list < shape.lists; ++list)
            {
                MTL4::CommandAllocator* allocator = m_device->newCommandAllocator();
                MTL4::CommandBuffer* commands = m_device->newCommandBuffer();
                m_calls.host += 2;
                if (allocator == nullptr || commands == nullptr)
                {
                    return std::unexpected(std::format("frame slot {}: command allocator or command buffer {}", index, list));
                }
                own(allocator);
                own(commands);
                slot.allocators.push_back(allocator);
                slot.lists.push_back(commands);
            }

            MTL4::ArgumentTableDescriptor* tableDescriptor = MTL4::ArgumentTableDescriptor::alloc()->init();
            tableDescriptor->setMaxBufferBindCount(kArgumentBuffers);
            tableDescriptor->setLabel(temporaryString(std::format("bench.arguments.{}", index)));
            NS::Error* error = nullptr;
            slot.table = m_device->newArgumentTable(tableDescriptor, &error);
            ++m_calls.host;
            tableDescriptor->release();
            if (slot.table == nullptr)
            {
                return std::unexpected(std::format("frame slot {}: argument table: {}", index, describe(error)));
            }
            own(slot.table);

            if (shape.arenaBytes > 0)
            {
                const std::expected<MTL::Buffer*, std::string> arena = sharedBuffer(shape.arenaBytes, std::format("bench.arena.{}", index));
                if (!arena)
                {
                    return std::unexpected(arena.error());
                }
                slot.arena = *arena;
                slot.arenaData = static_cast<std::byte*>(slot.arena->contents());
                slot.arenaAddress = slot.arena->gpuAddress();
                slot.arenaSize = shape.arenaBytes;
            }
        }
        m_residency->commit();
        ++m_calls.host;
        return {};
    }

    //==================================================================================================================
    NativeContext::Slot& NativeContext::beginFrame(uint32_t index, uint32_t commits)
    {
        if (m_slots.empty())
        {
            std::fprintf(stderr, "native frame %u began before prepareFrames\n", index);
            std::abort();
        }
        m_framePool = objc_autoreleasePoolPush();
        Slot& slot = m_slots[index % kFrameSlots];
        // The slot's last frame completed before the frame after it began, so its memory is free to reuse.
        for (MTL4::CommandAllocator* allocator : slot.allocators)
        {
            allocator->reset();
        }
        slot.arenaUsed = 0;
        m_collector->begin(commits);
        return slot;
    }

    //==================================================================================================================
    void NativeContext::commit(MTL4::CommandQueue* queue, std::span<MTL4::CommandBuffer* const> buffers)
    {
        MTL4::CommitOptions* options = MTL4::CommitOptions::alloc()->init();
        FeedbackCollector* collector = m_collector.get();
        const uint64_t frame = m_collector->frame();
        options->addFeedbackHandler(^(MTL4::CommitFeedback* feedback) {
          collector->report(frame, feedback->GPUStartTime(), feedback->GPUEndTime(), feedback->error() != nullptr);
        });
        m_pendingOptions.push_back(options);
        queue->commit(buffers.data(), buffers.size(), options);
        ++m_calls.submit;
    }

    //==================================================================================================================
    std::expected<std::optional<double>, std::string> NativeContext::endFrame()
    {
        const std::expected<double, std::string> span = waitForCommits();
        objc_autoreleasePoolPop(m_framePool);
        m_framePool = nullptr;
        if (!span)
        {
            return std::unexpected(span.error());
        }
        return *span;
    }

    //==================================================================================================================
    // Every commit since the collector began has reported, so their options are released; after a failed wait the GPU
    // may still report through them, and nothing is released.
    std::expected<double, std::string> NativeContext::waitForCommits()
    {
        std::expected<double, std::string> span = m_collector->wait();
        if (!span)
        {
            m_abandoned = true;
            return span;
        }
        for (MTL4::CommitOptions* options : m_pendingOptions)
        {
            options->release();
        }
        m_pendingOptions.clear();
        return span;
    }

    //==================================================================================================================
    std::expected<std::vector<std::byte>, std::string> NativeContext::readBuffer(MTL::Buffer* source, uint64_t offset, uint64_t size)
    {
        PoolScope pool;
        const std::expected<MTL::Buffer*, std::string> target = sharedBuffer(size, "bench.readback");
        if (!target)
        {
            return std::unexpected(target.error());
        }
        if (const std::expected<void, std::string> copied = runSetupCommit(source, offset, *target, 0, size); !copied)
        {
            return std::unexpected("readback: " + copied.error());
        }
        const auto* bytes = static_cast<const std::byte*>((*target)->contents());
        return std::vector<std::byte>(bytes, bytes + size);
    }

    //==================================================================================================================
    // One copy in its own command buffer, committed and waited; used for initial data and readback, outside every
    // measurement.
    std::expected<void, std::string> NativeContext::runSetupCommit(
        MTL::Buffer* source, uint64_t sourceOffset, MTL::Buffer* destination, uint64_t destinationOffset, uint64_t size
    )
    {
        PoolScope pool;
        m_residency->commit();
        m_setupAllocator->reset();
        ++m_calls.host;
        m_setupList->beginCommandBuffer(m_setupAllocator);
        MTL4::ComputeCommandEncoder* encoder = m_setupList->computeCommandEncoder();
        encoder->copyFromBuffer(source, sourceOffset, destination, destinationOffset, size);
        encoder->endEncoding();
        m_setupList->endCommandBuffer();
        m_calls.record += 5;
        m_collector->begin(1);
        MTL4::CommandBuffer* const buffers[] = { m_setupList };
        commit(m_graphics, buffers);
        const std::expected<double, std::string> span = waitForCommits();
        if (!span)
        {
            return std::unexpected(span.error());
        }
        return {};
    }
} // namespace rojoRHI::bench
