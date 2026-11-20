//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4ResidencyTests.cpp
/// @brief Tests the device residency set's deferred join, which a frame-arena page uses so that its claim takes no lock held across a commit.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Metal4Buffers.h"
#include "Metal4Common.h"
#include "Metal4Device.h"
#include "Metal4Residency.h"

#include <catch2/catch_test_macros.hpp>

#include <Metal/Metal.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

using namespace rojoRHI;

namespace {
    // A set of its own on the system device, and the small buffers it holds; the case holds the pool for its whole body.
    struct SetKit
    {
        SetKit()
        {
            device = MTL::CreateSystemDefaultDevice();
            REQUIRE(device != nullptr);
            REQUIRE(residency.create(device));
        }

        ~SetKit()
        {
            for (MTL::Buffer* buffer : buffers)
            {
                buffer->release();
            }
            residency.release();
            device->release();
        }

        SetKit(const SetKit&) = delete;
        SetKit& operator=(const SetKit&) = delete;

        MTL::Buffer* make()
        {
            MTL::Buffer* buffer = device->newBuffer(256, MTL::ResourceStorageModeShared);
            REQUIRE(buffer != nullptr);
            buffers.push_back(buffer);
            return buffer;
        }

        const metal4::AutoreleasePool pool;
        MTL::Device* device = nullptr;
        metal4::Residency residency;
        std::vector<MTL::Buffer*> buffers;
    };
} // namespace

TEST_CASE("a deferred add is not in the set until a commit, and is after it", "[metal4]")
{
    SetKit kit;
    MTL::Buffer* buffer = kit.make();
    metal4::Residency::Pending node;
    kit.residency.addDeferred(buffer, node);
    CHECK_FALSE(kit.residency.contains(buffer));
    CHECK(kit.residency.commitIfDirty());
    CHECK(kit.residency.contains(buffer));
    // The list was taken whole: the next commit has nothing to do.
    CHECK_FALSE(kit.residency.commitIfDirty());
}

TEST_CASE("an allocation removed before a commit joins and leaves, and its node is free to go", "[metal4]")
{
    SetKit kit;
    MTL::Buffer* kept = kit.make();
    MTL::Buffer* retired = kit.make();
    metal4::Residency::Pending keptNode;
    {
        metal4::Residency::Pending retiredNode;
        kit.residency.addDeferred(kept, keptNode);
        kit.residency.addDeferred(retired, retiredNode);
        kit.residency.remove(retired);
        // `retiredNode` goes out of scope here, as a retired page's payload does; the list holds none of it now.
    }
    CHECK_FALSE(kit.residency.contains(retired));
    // The retire drained the list, so the page deferred before it is in the set already, as a page of a list that never submitted.
    CHECK(kit.residency.contains(kept));
    CHECK(kit.residency.commitIfDirty());
    CHECK(kit.residency.contains(kept));
    CHECK_FALSE(kit.residency.contains(retired));
    CHECK_FALSE(kit.residency.commitIfDirty());
}

TEST_CASE("deferred adds pushed on several threads while others commit are in the set when their own commit returns", "[metal4]")
{
    constexpr uint32_t kThreads = 4;
    constexpr uint32_t kPerThread = 48;
    SetKit kit;
    std::vector<MTL::Buffer*> buffers;
    for (uint32_t i = 0; i < kThreads * kPerThread; ++i)
    {
        buffers.push_back(kit.make());
    }
    std::vector<metal4::Residency::Pending> nodes(buffers.size());
    std::atomic<bool> done = false;
    std::atomic<uint32_t> early = 0;
    std::vector<std::thread> pushers;
    for (uint32_t thread = 0; thread < kThreads; ++thread)
    {
        pushers.emplace_back([&, thread] {
            for (uint32_t i = 0; i < kPerThread; ++i)
            {
                const uint32_t index = thread * kPerThread + i;
                kit.residency.addDeferred(buffers[index], nodes[index]);
                // What a submission does after its list's claim: whatever commit it reaches or waits for has joined the page,
                // whether this thread, another pusher or a committer took the list.
                kit.residency.commitIfDirty();
                if (!kit.residency.contains(buffers[index]))
                {
                    early.fetch_add(1);
                }
            }
        });
    }
    std::vector<std::thread> committers;
    for (uint32_t i = 0; i < 2; ++i)
    {
        committers.emplace_back([&] {
            while (!done.load(std::memory_order_acquire))
            {
                kit.residency.commitIfDirty();
            }
        });
    }
    for (std::thread& pusher : pushers)
    {
        pusher.join();
    }
    done.store(true, std::memory_order_release);
    for (std::thread& committer : committers)
    {
        committer.join();
    }
    CHECK(early.load() == 0);
    kit.residency.commitIfDirty();
    for (MTL::Buffer* buffer : buffers)
    {
        CHECK(kit.residency.contains(buffer));
    }
    CHECK_FALSE(kit.residency.commitIfDirty());
}

TEST_CASE("a frame-arena page is resident after any queue's submission, though a list of another queue claimed it", "[metal4]")
{
    Device device = *createDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    // An upload larger than a standard page gets a page of its own, so each pass claims a new one.
    const std::vector<std::byte> bytes((2u << 20) + 5);
    for (const QueueType type : { QueueType::Graphics, QueueType::Compute, QueueType::Copy })
    {
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.claimer");
        (void)list.uploadFrameData(bytes, 16);
        MTL::Buffer* page = metal4::bufferPayload(impl.core, core::coreOf(list).page.buffer)->buffer;
        CHECK_FALSE(impl.residency.contains(page));
        // An empty submission still commits the set, on every queue.
        device.queue(type).submit({});
        CHECK(impl.residency.contains(page));
        list.end();
        device.queue(QueueType::Graphics).submit(std::array{ list });
        device.endFrame();
    }
    destroyDevice(device);
}
