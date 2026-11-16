//----------------------------------------------------------------------------------------------------------------------
/// @file SamplerTests.cpp
/// @brief Tests samplers, caller-owned slot ranges and their deferred reuse against chapter 04, part 2.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <utility>
#include <vector>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    Device makeDevice()
    {
        Result<Device> created = createDevice();
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    Texture makeTexture(Device device)
    {
        Result<Texture> created = device.createTexture({ .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled });
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    IndexRange reserveViews(Device device, uint32_t count)
    {
        Result<IndexRange> range = device.reserveViews(count);
        REQUIRE(range.has_value());
        return *range;
    }

    //==================================================================================================================
    IndexRange reserveSamplers(Device device, uint32_t count)
    {
        Result<IndexRange> range = device.reserveSamplers(count);
        REQUIRE(range.has_value());
        return *range;
    }

    //==================================================================================================================
    bool overlaps(IndexRange a, IndexRange b)
    {
        return a.base < b.base + b.count && b.base < a.base + a.count;
    }
} // namespace

//======================================================================================================================
TEST_CASE("createSampler returns distinct indices for live samplers", "[contract]")
{
    Device device = makeDevice();
    std::set<SamplerIndex> seen;
    for (int i = 0; i < 16; ++i)
    {
        Result<SamplerIndex> sampler = device.createSampler({});
        REQUIRE(sampler.has_value());
        CHECK(seen.insert(*sampler).second);
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("createSampler accepts the defaults, a comparison sampler and the anisotropy bounds", "[contract]")
{
    Device device = makeDevice();
    CHECK(device.createSampler({}).has_value());
    CHECK(device.createSampler({ .compareEnable = true, .compare = CompareOp::GreaterOrEqual, .label = "shadow" }).has_value());
    CHECK(device.createSampler({ .filter = SamplerFilter::Nearest, .mipFilter = SamplerFilter::Nearest, .address = SamplerAddress::ClampToEdge }).has_value());
    CHECK(device.createSampler({ .maxAnisotropy = 1 }).has_value());
    CHECK(device.createSampler({ .maxAnisotropy = 16 }).has_value());
    CHECK(device.createSampler({ .filter = SamplerFilter::Nearest, .maxAnisotropy = 1 }).has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a full sampler heap is an error result and a destroyed slot returns after its frame retires", "[contract]")
{
    Device device = makeDevice();
    const uint32_t capacity = device.capabilities().maxSamplers;
    std::vector<SamplerIndex> all;
    for (uint32_t i = 0; i < capacity; ++i)
    {
        Result<SamplerIndex> sampler = device.createSampler({});
        REQUIRE(sampler.has_value());
        all.push_back(*sampler);
    }
    CHECK_FALSE(device.createSampler({}).has_value());
    device.beginFrame();
    device.destroy(all.front());
    CHECK_FALSE(device.createSampler({}).has_value());
    device.endFrame();
    for (int i = 0; i < 3; ++i)
    {
        device.beginFrame();
        device.endFrame();
    }
    Result<SamplerIndex> reused = device.createSampler({});
    REQUIRE(reused.has_value());
    CHECK(*reused == all.front());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("reserved ranges are contiguous, carry their kind and do not overlap", "[contract]")
{
    Device device = makeDevice();
    const IndexRange first = reserveViews(device, 8);
    const IndexRange second = reserveViews(device, 4);
    CHECK(first.count == 8);
    CHECK(first.kind == IndexRange::Kind::Views);
    CHECK(second.count == 4);
    CHECK_FALSE(overlaps(first, second));
    const IndexRange samplers = reserveSamplers(device, 3);
    CHECK(samplers.count == 3);
    CHECK(samplers.kind == IndexRange::Kind::Samplers);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a range does not overlap the slots createView and createSampler hand out", "[contract]")
{
    Device device = makeDevice();
    const Texture texture = makeTexture(device);
    const ViewIndex single = *device.createView(texture, {});
    const IndexRange range = reserveViews(device, 5);
    const ViewIndex after = *device.createView(texture, {});
    CHECK_FALSE(overlaps(range, { std::to_underlying(single), 1 }));
    CHECK_FALSE(overlaps(range, { std::to_underlying(after), 1 }));
    const SamplerIndex sampler = *device.createSampler({});
    const IndexRange samplers = reserveSamplers(device, 2);
    CHECK_FALSE(overlaps(samplers, { std::to_underlying(sampler), 1, IndexRange::Kind::Samplers }));
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a reservation larger than the heap is an error result", "[contract]")
{
    Device device = makeDevice();
    const Capabilities& capabilities = device.capabilities();
    CHECK_FALSE(device.reserveViews(capabilities.maxViews + 1).has_value());
    CHECK_FALSE(device.reserveSamplers(capabilities.maxSamplers + 1).has_value());
    const IndexRange all = reserveSamplers(device, capabilities.maxSamplers);
    CHECK_FALSE(device.reserveSamplers(1).has_value());
    CHECK_FALSE(device.createSampler({}).has_value());
    device.release(all);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a released range is not handed out again before its frame retires", "[contract]")
{
    Device device = makeDevice();
    const uint32_t capacity = device.capabilities().maxSamplers;
    const IndexRange all = reserveSamplers(device, capacity);
    device.beginFrame();
    device.release(all);
    CHECK_FALSE(device.reserveSamplers(1).has_value());
    device.endFrame();
    for (int i = 0; i < 2; ++i)
    {
        device.beginFrame();
        CHECK_FALSE(device.reserveSamplers(1).has_value());
        device.endFrame();
    }
    device.beginFrame();
    CHECK(device.reserveSamplers(capacity).has_value());
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a released view range is not handed out again before its frame retires", "[contract]")
{
    Device device = makeDevice();
    device.beginFrame();
    const IndexRange range = reserveViews(device, 16);
    device.release(range);
    for (int i = 0; i < 8; ++i)
    {
        CHECK_FALSE(overlaps(reserveViews(device, 2), range));
    }
    device.endFrame();
    device.beginFrame();
    for (int i = 0; i < 8; ++i)
    {
        CHECK_FALSE(overlaps(reserveViews(device, 2), range));
    }
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a view written into a range keeps its texture alive until the slot is replaced or the range is released", "[contract]")
{
    Device device = makeDevice();
    const Texture first = makeTexture(device);
    const Texture second = makeTexture(device);
    const IndexRange range = reserveViews(device, 2);
    device.writeView(range, 0, first, {});
    device.writeView(range, 1, second, {});
    device.writeView(range, 0, second, {});
    device.destroy(first);
    device.release(range);
    device.destroy(second);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("every slot of a range accepts a write and a rewrite", "[contract]")
{
    Device device = makeDevice();
    const Texture texture = makeTexture(device);
    const IndexRange views = reserveViews(device, 3);
    for (uint32_t i = 0; i < views.count; ++i)
    {
        device.writeView(views, i, texture, {});
        device.writeView(views, i, texture, { .usage = TextureUsage::Sampled });
    }
    const IndexRange samplers = reserveSamplers(device, 3);
    for (uint32_t i = 0; i < samplers.count; ++i)
    {
        device.writeSampler(samplers, i, {});
        device.writeSampler(samplers, i, { .compareEnable = true, .compare = CompareOp::Less });
    }
    device.release(views);
    device.release(samplers);
    device.destroy(texture);
    destroyDevice(device);
}
