//----------------------------------------------------------------------------------------------------------------------
/// @file FormatTests.cpp
/// @brief Tests the format enumeration and the capability rules chapter 15 states for every backend.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <type_traits>

using namespace rojoRHI;

namespace {
    /// One cell of the guaranteed-capabilities table: the floor requires it, the reference forbids it, or the device reports it.
    enum class Cell
    {
        Yes,
        No,
        Reported
    };

    struct FloorRow
    {
        Format format;
        Cell sampled;
        Cell filterLinear;
        Cell storageRead;
        Cell storageWrite;
        Cell color; ///< The "color attachment and blend" column: both members.
        Cell depth;
    };

    constexpr Cell Y = Cell::Yes;
    constexpr Cell N = Cell::No;
    constexpr Cell R = Cell::Reported;

    // Transcribed from the "Guaranteed capabilities" table of chapter 15.
    constexpr std::array<FloorRow, 13> kFloor = { {
        { Format::BGRA8Unorm, Y, Y, R, R, Y, N },
        { Format::RGBA8Unorm, Y, Y, Y, Y, Y, N },
        { Format::RGBA8Unorm_sRGB, Y, Y, N, N, Y, N },
        { Format::RGBA16Float, Y, Y, Y, Y, Y, N },
        { Format::RG16Float, Y, Y, R, Y, Y, N },
        { Format::R8Unorm, Y, Y, Y, Y, Y, N },
        { Format::BC1Unorm, Y, Y, N, N, N, N },
        { Format::BC1Unorm_sRGB, Y, Y, N, N, N, N },
        { Format::D32Float, Y, R, N, N, N, Y },
        { Format::R16Float, Y, Y, Y, Y, Y, N },
        { Format::R32Float, Y, R, Y, Y, R, N },
        { Format::D32FloatS8, Y, R, N, N, N, Y },
        { Format::RGB10A2Unorm, Y, Y, R, R, Y, N },
    } };

    //==================================================================================================================
    void checkCell(Cell cell, bool reported)
    {
        if (cell == Cell::Yes)
        {
            CHECK(reported);
        }
        else if (cell == Cell::No)
        {
            CHECK_FALSE(reported);
        }
    }
} // namespace

//======================================================================================================================
TEST_CASE("Format enumerators keep the values chapter 15 lists", "[contract]")
{
    static_assert(std::is_same_v<std::underlying_type_t<Format>, uint32_t>);
    CHECK(static_cast<uint32_t>(Format::Undefined) == 0);
    CHECK(static_cast<uint32_t>(Format::BGRA8Unorm) == 1);
    CHECK(static_cast<uint32_t>(Format::RGBA8Unorm) == 2);
    CHECK(static_cast<uint32_t>(Format::RGBA8Unorm_sRGB) == 3);
    CHECK(static_cast<uint32_t>(Format::RGBA16Float) == 4);
    CHECK(static_cast<uint32_t>(Format::RG16Float) == 5);
    CHECK(static_cast<uint32_t>(Format::R8Unorm) == 6);
    CHECK(static_cast<uint32_t>(Format::BC1Unorm) == 7);
    CHECK(static_cast<uint32_t>(Format::BC1Unorm_sRGB) == 8);
    CHECK(static_cast<uint32_t>(Format::D32Float) == 9);
    CHECK(static_cast<uint32_t>(Format::R16Float) == 10);
    CHECK(static_cast<uint32_t>(Format::R32Float) == 11);
    CHECK(static_cast<uint32_t>(Format::D32FloatS8) == 12);
    CHECK(static_cast<uint32_t>(Format::RGB10A2Unorm) == 13);
}

//======================================================================================================================
TEST_CASE("a default FormatCapabilities supports nothing and one sample", "[contract]")
{
    const FormatCapabilities caps;
    CHECK_FALSE(caps.sampled);
    CHECK_FALSE(caps.filterLinear);
    CHECK_FALSE(caps.storageRead);
    CHECK_FALSE(caps.storageWrite);
    CHECK_FALSE(caps.colorAttachment);
    CHECK_FALSE(caps.blend);
    CHECK_FALSE(caps.depthAttachment);
    CHECK_FALSE(caps.resolve);
    CHECK_FALSE(caps.sparse);
    CHECK(caps.sampleCounts == 1);
}

//======================================================================================================================
TEST_CASE("every format reports at least its guaranteed capabilities and none the reference forbids", "[contract]")
{
    Device device = *createDevice();
    for (const FloorRow& row : kFloor)
    {
        CAPTURE(static_cast<uint32_t>(row.format));
        const FormatCapabilities caps = device.formatCapabilities(row.format);
        checkCell(row.sampled, caps.sampled);
        checkCell(row.filterLinear, caps.filterLinear);
        checkCell(row.storageRead, caps.storageRead);
        checkCell(row.storageWrite, caps.storageWrite);
        checkCell(row.color, caps.colorAttachment);
        checkCell(row.color == Cell::Yes || row.color == Cell::No ? row.color : Cell::Reported, caps.blend);
        checkCell(row.depth, caps.depthAttachment);
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a capability that depends on an attachment role is reported only with that role", "[contract]")
{
    Device device = *createDevice();
    for (const FloorRow& row : kFloor)
    {
        CAPTURE(static_cast<uint32_t>(row.format));
        const FormatCapabilities caps = device.formatCapabilities(row.format);
        if (caps.blend || caps.resolve)
        {
            CHECK(caps.colorAttachment);
        }
        CHECK_FALSE((caps.colorAttachment && caps.depthAttachment));
        CHECK((caps.sampleCounts & 1u) == 1u);
        if (!caps.colorAttachment && !caps.depthAttachment)
        {
            CHECK(caps.sampleCounts == 1);
        }
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("formatCapabilities returns the same value on every call", "[contract]")
{
    Device device = *createDevice();
    for (const FloorRow& row : kFloor)
    {
        const FormatCapabilities first = device.formatCapabilities(row.format);
        device.beginFrame();
        device.endFrame();
        const FormatCapabilities second = device.formatCapabilities(row.format);
        CHECK(first.sampled == second.sampled);
        CHECK(first.sampleCounts == second.sampleCounts);
        CHECK(first.storageWrite == second.storageWrite);
    }
    destroyDevice(device);
}
