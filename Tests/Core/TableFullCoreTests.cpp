//----------------------------------------------------------------------------------------------------------------------
/// @file TableFullCoreTests.cpp
/// @brief Tests that a creation which finds its pool's native table full fails with OutOfMemory and leaves no trace.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/Buffers.h"
#include "Core/DeviceCore.h"
#include "Core/Textures.h"
#include "Core/Upscalers.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

using namespace rojoRHI;

namespace {
    /// Three slots: slot zero and two objects per type.
    constexpr uint32_t kSmallPool = 3;

    /// The counters a creation moves.
    struct CreationCounts
    {
        uint64_t nativeCreateCalls = 0;
        uint64_t gpuAllocations = 0;
        friend bool operator==(const CreationCounts&, const CreationCounts&) = default;
    };

    //==================================================================================================================
    CreationCounts creationCounts(const core::DeviceCore& core)
    {
        const Counters counters = core.counters();
        return { counters.nativeCreateCalls, counters.gpuAllocations };
    }

    //==================================================================================================================
    uint64_t placedIn(core::DeviceCore& core, Heap heap)
    {
        return core.pool<Heap>().lookup(heap).record->placedResources;
    }

    //==================================================================================================================
    /// Creates an upscaler as a backend with no native object does: prepares the record, then allocates it.
    Result<Upscaler> createUpscaler(core::DeviceCore& core, const UpscalerDesc& desc)
    {
        Result<core::UpscalerRecord> record = core::prepareUpscaler(core, desc);
        if (!record)
        {
            return std::unexpected(std::move(record.error()));
        }
        const Upscaler upscaler = core.pool<Upscaler>().allocate(std::move(*record));
        if (upscaler.bits == 0)
        {
            return std::unexpected(core::tableFull("upscaler", core.pool<Upscaler>().capacity()));
        }
        return upscaler;
    }

    //==================================================================================================================
    core::BufferRecord bufferRecord(Heap heap = {})
    {
        return core::BufferRecord{ .label = "full.buffer", .size = 256, .heap = heap };
    }

    //==================================================================================================================
    core::TextureRecord textureRecord(Heap heap = {})
    {
        return core::TextureRecord{
            .label = "full.texture", .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled, .heap = heap, .heapSize = 256
        };
    }
} // namespace

//======================================================================================================================
TEST_CASE("a buffer that finds its table full is OutOfMemory and leaves no record, placement, tracking or count", "[core]")
{
    core::DeviceCore core{ DeviceDesc{ .validation = true }, kSmallPool };
    const Heap kept = core.pool<Heap>().allocate({ .label = "full.kept", .size = 4096 });
    const Heap spare = core.pool<Heap>().allocate({ .label = "full.spare", .size = 4096 });
    REQUIRE(core::addBuffer(core, bufferRecord()).has_value());
    REQUIRE(core::addBuffer(core, bufferRecord(kept)).has_value());
    const CreationCounts before = creationCounts(core);

    const Result<Buffer> full = core::addBuffer(core, bufferRecord(spare));
    REQUIRE_FALSE(full.has_value());
    CHECK(full.error().category == ErrorCategory::OutOfMemory);
    CHECK(full.error().message == "the buffer table of 2 objects is full");
    CHECK(core.pool<Buffer>().liveCount() == 2);
    CHECK(placedIn(core, kept) == 1);
    CHECK(placedIn(core, spare) == 0);
    CHECK(creationCounts(core) == before);
#ifndef NDEBUG
    CHECK(core.tracker().heapSegmentCount(kept).has_value());
    CHECK(core.tracker().heapSegmentCount(spare) == std::nullopt);
    CHECK(core.tracker().segmentCount(Buffer{}) == std::nullopt);
#endif
    core.beginDestroy();
}

//======================================================================================================================
TEST_CASE("a texture that finds its table full is OutOfMemory and leaves no record, placement, tracking or count", "[core]")
{
    core::DeviceCore core{ DeviceDesc{ .validation = true }, kSmallPool };
    const Heap kept = core.pool<Heap>().allocate({ .label = "full.kept", .size = 4096 });
    const Heap spare = core.pool<Heap>().allocate({ .label = "full.spare", .size = 4096 });
    REQUIRE(core::addTexture(core, textureRecord()).has_value());
    REQUIRE(core::addTexture(core, textureRecord(kept)).has_value());
    const CreationCounts before = creationCounts(core);

    const Result<Texture> full = core::addTexture(core, textureRecord(spare));
    REQUIRE_FALSE(full.has_value());
    CHECK(full.error().category == ErrorCategory::OutOfMemory);
    CHECK(full.error().message == "the texture table of 2 objects is full");
    CHECK(core.pool<Texture>().liveCount() == 2);
    CHECK(placedIn(core, kept) == 1);
    CHECK(placedIn(core, spare) == 0);
    CHECK(creationCounts(core) == before);
#ifndef NDEBUG
    CHECK(core.tracker().heapSegmentCount(kept).has_value());
    CHECK(core.tracker().heapSegmentCount(spare) == std::nullopt);
    CHECK(core.tracker().aliasMarkCount(Texture{}) == std::nullopt);
#endif
    core.beginDestroy();
}

//======================================================================================================================
TEST_CASE("a frame-arena page that finds the buffer table full is OutOfMemory and counts nothing", "[core]")
{
    core::DeviceCore core{ DeviceDesc{}, kSmallPool };
    REQUIRE(core::addFrameArenaPage(core, bufferRecord()).has_value());
    REQUIRE(core::addFrameArenaPage(core, bufferRecord()).has_value());
    const CreationCounts before = creationCounts(core);

    const Result<Buffer> full = core::addFrameArenaPage(core, bufferRecord());
    REQUIRE_FALSE(full.has_value());
    CHECK(full.error().category == ErrorCategory::OutOfMemory);
    CHECK(core.pool<Buffer>().liveCount() == 2);
    CHECK(creationCounts(core) == before);
    core.beginDestroy();
}

//======================================================================================================================
TEST_CASE("an upscaler that finds its table full is OutOfMemory and counts nothing", "[core]")
{
    static constexpr std::array<UpscalerKind, 1> kOffered = { UpscalerKind::FSR };
    core::DeviceCore core{ DeviceDesc{}, kSmallPool };
    core.capabilities().upscalers = kOffered;
    const UpscalerDesc desc{ .kind = UpscalerKind::FSR,
                             .input = { 40, 30 },
                             .output = { 80, 60 },
                             .color = Format::RGBA8Unorm,
                             .depth = Format::D32FloatS8,
                             .motion = Format::RG16Float,
                             .outputFormat = Format::BGRA8Unorm,
                             .label = "full.upscaler" };
    REQUIRE(createUpscaler(core, desc).has_value());
    REQUIRE(createUpscaler(core, desc).has_value());
    const CreationCounts before = creationCounts(core);

    const Result<Upscaler> full = createUpscaler(core, desc);
    REQUIRE_FALSE(full.has_value());
    CHECK(full.error().category == ErrorCategory::OutOfMemory);
    CHECK(full.error().message == "the upscaler table of 2 objects is full");
    CHECK(core.pool<Upscaler>().liveCount() == 2);
    CHECK(creationCounts(core) == before);
    core.beginDestroy();
}
