//----------------------------------------------------------------------------------------------------------------------
/// @file HeapCoreTests.cpp
/// @brief Tests the record the core keeps for a heap and the placed resources it counts.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"

#include <catch2/catch_test_macros.hpp>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    uint64_t placedIn(Device device, Heap heap)
    {
        return core::coreOf(device).pool<Heap>().lookup(heap).record->placedResources;
    }
} // namespace

//======================================================================================================================
TEST_CASE("a heap's record keeps its label, size and memory class", "[core]")
{
    Device device = *createDevice();
    const Heap heap = *device.createHeap({ .size = 8192, .memory = Memory::Upload, .label = "test.heap" });
    const auto found = core::coreOf(device).pool<Heap>().lookup(heap);
    REQUIRE(found.record != nullptr);
    CHECK(found.record->label == "test.heap");
    CHECK(found.record->size == 8192);
    CHECK(found.record->memory == Memory::Upload);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a heap without a label is recorded as rojorhi.heap.unnamed", "[core]")
{
    Device device = *createDevice();
    const Heap heap = *device.createHeap({ .size = 64 });
    CHECK(core::coreOf(device).pool<Heap>().lookup(heap).record->label == "rojorhi.heap.unnamed");
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a heap counts the live buffers placed in it", "[core]")
{
    Device device = *createDevice();
    const Heap heap = *device.createHeap({ .size = 4096 });
    CHECK(placedIn(device, heap) == 0);
    const Buffer first = *device.createBuffer({ .size = 64 }, {}, { heap, 0 });
    const Buffer second = *device.createBuffer({ .size = 64 }, {}, { heap, 256 });
    CHECK(placedIn(device, heap) == 2);
    device.destroy(first);
    CHECK(placedIn(device, heap) == 1);
    device.destroy(second);
    CHECK(placedIn(device, heap) == 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a dedicated buffer is counted by no heap", "[core]")
{
    Device device = *createDevice();
    const Heap heap = *device.createHeap({ .size = 4096 });
    const Buffer buffer = *device.createBuffer({ .size = 64 });
    CHECK(placedIn(device, heap) == 0);
    CHECK(core::coreOf(device).pool<Buffer>().lookup(buffer).record->heap == Heap{});
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a placed buffer's record names its heap", "[core]")
{
    Device device = *createDevice();
    const Heap heap = *device.createHeap({ .size = 4096 });
    const Buffer buffer = *device.createBuffer({ .size = 64 }, {}, { heap, 0 });
    CHECK(core::coreOf(device).pool<Buffer>().lookup(buffer).record->heap == heap);
    destroyDevice(device);
}
