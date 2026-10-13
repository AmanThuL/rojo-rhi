//----------------------------------------------------------------------------------------------------------------------
/// @file HeapDeathTests.cpp
/// @brief Death tests for the heap and placement rules and for destroying a heap that still holds resources.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Death.h"

#include <catch2/catch_test_macros.hpp>

using namespace rojoRHI;

//======================================================================================================================
TEST_CASE("VU-rojo-heap-0001 a heap has size zero", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-heap-0001", [] {
        Device device = *createDevice();
        (void)device.createHeap({ .size = 0 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-heap-0002 placement requirements are requested for a Sparse buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-heap-0002", [] {
        Device device = *createDevice();
        (void)device.placementRequirements(BufferDesc{ .size = 64, .usage = BufferUsage::Sparse });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-heap-0003 a buffer extends past the end of its heap", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-heap-0003", [] {
        Device device = *createDevice();
        const BufferDesc desc{ .size = 512 };
        const PlacementRequirements requirements = device.placementRequirements(desc);
        const Heap heap = *device.createHeap({ .size = requirements.size });
        (void)device.createBuffer(desc, {}, { heap, requirements.alignment });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-heap-0004 a buffer is placed at a misaligned offset", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-heap-0004", [] {
        Device device = *createDevice();
        const BufferDesc desc{ .size = 512 };
        const PlacementRequirements requirements = device.placementRequirements(desc);
        const Heap heap = *device.createHeap({ .size = requirements.size + requirements.alignment * 2 });
        (void)device.createBuffer(desc, {}, { heap, 1 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-heap-0005 a buffer is placed in a heap of another memory class", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-heap-0005", [] {
        Device device = *createDevice();
        const Heap heap = *device.createHeap({ .size = 4096, .memory = Memory::Upload });
        (void)device.createBuffer({ .size = 64, .memory = Memory::Gpu }, {}, { heap, 0 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-heap-0006 a Sparse buffer is placed in a heap", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-heap-0006", [] {
        Device device = *createDevice();
        const Heap heap = *device.createHeap({ .size = 4096 });
        (void)device.createBuffer({ .size = 64, .usage = BufferUsage::Sparse }, {}, { heap, 0 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-device-0008 a heap is destroyed while a buffer is placed in it", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-device-0008", [] {
        Device device = *createDevice();
        const Heap heap = *device.createHeap({ .size = 4096 });
        (void)device.createBuffer({ .size = 64 }, {}, { heap, 0 });
        device.destroy(heap);
    });
}
