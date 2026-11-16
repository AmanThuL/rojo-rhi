//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4AutoreleaseTests.cpp
/// @brief Tests that the Metal 4 entries which hand a label to the driver leave nothing in the caller's autorelease pool.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Metal4Device.h"
#include "Metal4Encoders.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <dlfcn.h>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    /// Runs `body` under a pool of its own and returns whether it autoreleased anything into it. A pool pushed and popped
    /// first marks where the next boundary lands when nothing sits in between, so the probe boundary pushed after the body
    /// lands there too unless the body added an object.
    template<typename Body> bool autoreleasesInto(Body&& body)
    {
        // The runtime's own entries, since the pool tokens are what show where an object lands.
        using Push = void* (*)();
        using Pop = void (*)(void*);
        const auto poolPush = reinterpret_cast<Push>(dlsym(RTLD_DEFAULT, "objc_autoreleasePoolPush"));
        const auto poolPop = reinterpret_cast<Pop>(dlsym(RTLD_DEFAULT, "objc_autoreleasePoolPop"));
        REQUIRE(poolPush != nullptr);
        REQUIRE(poolPop != nullptr);
        void* const outer = poolPush();
        void* const empty = poolPush();
        poolPop(empty);
        body();
        void* const probe = poolPush();
        const bool leaked = probe != empty;
        poolPop(outer);
        return leaked;
    }

    //==================================================================================================================
    /// Records a list with `label` on the list, a scope, a copy and a render pass with a split, and checks that no entry
    /// leaves an object in the caller's pool.
    void checkLabelsLeaveNothing(const char* label)
    {
        Device device = *createDevice();
        const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
        const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
        const Texture target = *device.createTexture(
            TextureDesc{
                .type = TextureType::Tex2D, .format = Format::RGBA8Unorm, .extent = { 16, 16, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" }
        );
        const std::array colors{ ColorAttachment{ .texture = target } };
        device.beginFrame();
        CommandList list;
        CHECK_FALSE(autoreleasesInto([&] { list = device.acquireCommandList(QueueType::Graphics, label); }));
        CHECK_FALSE(autoreleasesInto([&] { list.beginScope(label); }));
        CHECK_FALSE(autoreleasesInto([&] { list.copyBuffer(a, 0, b, 0, 256); }));
        CHECK_FALSE(autoreleasesInto([&] { list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, 16, 16 }, .label = label }); }));
        // A scope inside the pass opens the render encoder, labelled after the pass.
        CHECK_FALSE(autoreleasesInto([&] { list.beginScope(label); }));
        CHECK_FALSE(autoreleasesInto([&] { metal4::splitPass(CommandListImpl::of(list)); }));
        // Ending the scope resumes the pass: a new encoder opens and the open group is pushed on it again.
        CHECK_FALSE(autoreleasesInto([&] { list.endScope(); }));
        CHECK_FALSE(autoreleasesInto([&] { list.endRenderPass(); }));
        CHECK_FALSE(autoreleasesInto([&] { list.endScope(); }));
        CHECK_FALSE(autoreleasesInto([&] { list.end(); }));
        const std::array<CommandList, 1> lists{ list };
        device.queue(QueueType::Graphics).submit(lists);
        device.endFrame();
        destroyDevice(device);
    }
} // namespace

//======================================================================================================================
TEST_CASE("a short label reaches the driver inside a pool of the call's own", "[metal4]")
{
    // A short label is a tagged-pointer string, and the driver reads its text through an autoreleased container.
    checkLabelsLeaveNothing("l");
}

//======================================================================================================================
TEST_CASE("a non-ASCII label reaches the driver inside a pool of the call's own", "[metal4]")
{
    checkLabelsLeaveNothing("p\xC3\xA4sse");
}

//======================================================================================================================
TEST_CASE("a long label reaches the driver inside a pool of the call's own", "[metal4]")
{
    checkLabelsLeaveNothing("test.a.label.much.longer.than.any.tagged.pointer.string.can.hold");
}

//======================================================================================================================
TEST_CASE("ending a command list leaves nothing in the caller's pool", "[metal4]")
{
    Device device = *createDevice();
    const Heap heap = *device.createHeap(HeapDesc{ .size = 64 * 65536, .memory = Memory::Gpu, .label = "test.heap" });
    const Buffer sparse = *device.createBuffer(BufferDesc{ .size = 4 * 65536, .memory = Memory::Gpu, .usage = BufferUsage::Sparse, .label = "test.sparse" });
    const Buffer source = *device.createBuffer({ .size = 256, .label = "test.source" });
    device.beginFrame();
    for (const int count : { 1, 10, 100 })
    {
        int leaks = 0;
        for (int i = 0; i < count; ++i)
        {
            // A new mapping puts a first use in front of the copy, which the list orders with a compute encoder.
            device.queue(QueueType::Graphics).mapTiles(std::array{ TileMapping{ .buffer = sparse, .bufferSize = 65536, .heap = heap } });
            CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
            list.barrier(std::array{ GlobalBarrier{ Stage::Copy, Access::None, Stage::Copy, Access::CopyWrite } });
            list.copyBuffer(source, 0, sparse, 0, 256);
            leaks += autoreleasesInto([&] { list.end(); }) ? 1 : 0;
            const std::array<CommandList, 1> lists{ list };
            device.queue(QueueType::Graphics).submit(lists);
        }
        WARN("ends " << count << " leaks " << leaks);
        CHECK(leaks == 0);
    }
    device.endFrame();
    destroyDevice(device);
}
