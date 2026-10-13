//----------------------------------------------------------------------------------------------------------------------
/// @file NullHeapTests.cpp
/// @brief Tests what only the Null backend promises about heaps: its placement requirements.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

using namespace rojoRHI;

//======================================================================================================================
TEST_CASE("the Null device places a buffer at 256 bytes and rounds its size up to 256", "[null]")
{
    Device device = *createDevice();
    const PlacementRequirements small = device.placementRequirements(BufferDesc{ .size = 1 });
    CHECK(small.size == 256);
    CHECK(small.alignment == 256);
    CHECK(device.placementRequirements(BufferDesc{ .size = 256 }).size == 256);
    CHECK(device.placementRequirements(BufferDesc{ .size = 257 }).size == 512);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the Null device's placement requirements do not depend on the memory class", "[null]")
{
    Device device = *createDevice();
    const PlacementRequirements gpu = device.placementRequirements(BufferDesc{ .size = 300, .memory = Memory::Gpu });
    const PlacementRequirements upload = device.placementRequirements(BufferDesc{ .size = 300, .memory = Memory::Upload });
    CHECK(gpu.size == upload.size);
    CHECK(gpu.alignment == upload.alignment);
    destroyDevice(device);
}
