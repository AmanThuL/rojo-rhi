//----------------------------------------------------------------------------------------------------------------------
/// @file HeapDeathTests.cpp
/// @brief Death tests for the heap and placement rules and for destroying a heap that still holds resources.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
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
        core::coreOf(device).capabilities().placementSparse = true;
        (void)device.placementRequirements(BufferDesc{ .size = 64, .usage = BufferUsage::Sparse });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-heap-0002 placement requirements are requested for a Sparse texture", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-heap-0002", [] {
        Device device = *createDevice();
        core::DeviceCore& core = core::coreOf(device);
        core.capabilities().placementSparse = true;
        // The format must report sparse residency too, or VU-rojo-format-0003 fires first.
        FormatCapabilities rgba = core.formatCapabilities(Format::RGBA8Unorm);
        rgba.sparse = true;
        core.setFormatCapabilities(Format::RGBA8Unorm, rgba);
        (void)device.placementRequirements(
            TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled | TextureUsage::Sparse }
        );
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
        // A backend may round the heap up, so the offset is an aligned one far past any rounding.
        (void)device.createBuffer(desc, {}, { heap, 1ull << 40 });
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
TEST_CASE("VU-rojo-heap-0005 a texture is placed in an Upload heap", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-heap-0005", [] {
        Device device = *createDevice();
        const Heap heap = *device.createHeap({ .size = 1 << 20, .memory = Memory::Upload });
        (void)device.createTexture({ .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled }, {}, { heap, 0 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-heap-0006 a Sparse buffer is placed in a heap", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-heap-0006", [] {
        Device device = *createDevice();
        core::coreOf(device).capabilities().placementSparse = true;
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
