//----------------------------------------------------------------------------------------------------------------------
/// @file HeapTests.cpp
/// @brief Tests heap creation, placement requirements and placed buffers against chapter 03 part 2.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <cstdint>

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
    Heap makeHeap(Device device, HeapDesc desc)
    {
        Result<Heap> created = device.createHeap(desc);
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    bool isPowerOfTwo(uint64_t value)
    {
        return value != 0 && (value & (value - 1)) == 0;
    }
} // namespace

//======================================================================================================================
TEST_CASE("a heap is created in every memory class", "[contract]")
{
    Device device = makeDevice();
    for (const Memory memory : { Memory::Gpu, Memory::Upload, Memory::Readback })
    {
        CHECK(device.createHeap({ .size = 4096, .memory = memory }).has_value());
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("placement requirements have a power-of-two alignment and cover the buffer", "[contract]")
{
    Device device = makeDevice();
    for (const uint64_t size : { 1ull, 100ull, 256ull, 4097ull })
    {
        const PlacementRequirements requirements = device.placementRequirements(BufferDesc{ .size = size });
        CHECK(isPowerOfTwo(requirements.alignment));
        CHECK(requirements.size >= size);
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("placement requirements of equal descriptors are equal", "[contract]")
{
    Device device = makeDevice();
    const BufferDesc desc{ .size = 1000, .usage = BufferUsage::ShaderWrite };
    const PlacementRequirements first = device.placementRequirements(desc);
    const PlacementRequirements second = device.placementRequirements(desc);
    CHECK(first.size == second.size);
    CHECK(first.alignment == second.alignment);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a buffer is placed at an aligned offset inside a heap", "[contract]")
{
    Device device = makeDevice();
    const BufferDesc desc{ .size = 1000 };
    const PlacementRequirements requirements = device.placementRequirements(desc);
    // The size need not be a multiple of the alignment, so the second offset rounds it up.
    const uint64_t second = (requirements.size + requirements.alignment - 1) / requirements.alignment * requirements.alignment;
    const Heap heap = makeHeap(device, { .size = second + requirements.size });
    CHECK(device.createBuffer(desc, {}, { heap, 0 }).has_value());
    CHECK(device.createBuffer(desc, {}, { heap, second }).has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("buffers placed over the same bytes alias without a diagnostic", "[contract]")
{
    Device device = makeDevice();
    const BufferDesc desc{ .size = 512 };
    const Heap heap = makeHeap(device, { .size = device.placementRequirements(desc).size });
    CHECK(device.createBuffer(desc, {}, { heap, 0 }).has_value());
    CHECK(device.createBuffer(desc, {}, { heap, 0 }).has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a buffer fills a heap exactly", "[contract]")
{
    Device device = makeDevice();
    const BufferDesc desc{ .size = 512 };
    const Heap heap = makeHeap(device, { .size = device.placementRequirements(desc).size });
    CHECK(device.createBuffer(desc, {}, { heap, 0 }).has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an Upload buffer placed in an Upload heap has a CPU pointer", "[contract]")
{
    Device device = makeDevice();
    const BufferDesc desc{ .size = 64, .memory = Memory::Upload };
    const Heap heap = makeHeap(device, { .size = device.placementRequirements(desc).size, .memory = Memory::Upload });
    const Result<Buffer> buffer = device.createBuffer(desc, {}, { heap, 0 });
    REQUIRE(buffer.has_value());
    CHECK(device.cpuPointer(*buffer) != nullptr);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a heap is destroyed after the buffers placed in it", "[contract]")
{
    Device device = makeDevice();
    const BufferDesc desc{ .size = 64 };
    const Heap heap = makeHeap(device, { .size = device.placementRequirements(desc).size });
    const Buffer buffer = *device.createBuffer(desc, {}, { heap, 0 });
    device.destroy(buffer);
    device.destroy(heap);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a heap is destroyed once its resources were destroyed and recreated", "[contract]")
{
    Device device = makeDevice();
    const BufferDesc desc{ .size = 64 };
    const Heap heap = makeHeap(device, { .size = device.placementRequirements(desc).size });
    device.destroy(*device.createBuffer(desc, {}, { heap, 0 }));
    device.destroy(*device.createBuffer(desc, {}, { heap, 0 }));
    device.destroy(heap);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("destroyDevice releases a heap that still holds placed buffers", "[contract]")
{
    Device device = makeDevice();
    const BufferDesc desc{ .size = 64 };
    const Heap heap = makeHeap(device, { .size = device.placementRequirements(desc).size });
    (void)device.createBuffer(desc, {}, { heap, 0 });
    destroyDevice(device);
}
