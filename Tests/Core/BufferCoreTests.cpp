//----------------------------------------------------------------------------------------------------------------------
/// @file BufferCoreTests.cpp
/// @brief Tests the record the core keeps for a buffer and the label default.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"

#include <catch2/catch_test_macros.hpp>

#include <cstddef>

using namespace rojoRHI;

//======================================================================================================================
TEST_CASE("a buffer without a label is recorded as rojorhi.buffer.unnamed", "[core]")
{
    Device device = *createDevice();
    const Buffer buffer = *device.createBuffer({ .size = 8 });
    const auto found = core::coreOf(device).pool<Buffer>().lookup(buffer);
    REQUIRE(found.record != nullptr);
    CHECK(found.record->label == "rojorhi.buffer.unnamed");
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a buffer's record keeps its label, size, memory and usage", "[core]")
{
    Device device = *createDevice();
    const Buffer buffer = *device.createBuffer({ .size = 96, .memory = Memory::Gpu, .usage = BufferUsage::ShaderWrite, .label = "test.buffer" });
    const auto found = core::coreOf(device).pool<Buffer>().lookup(buffer);
    REQUIRE(found.record != nullptr);
    CHECK(found.record->label == "test.buffer");
    CHECK(found.record->size == 96);
    CHECK(found.record->memory == Memory::Gpu);
    CHECK(found.record->usage == BufferUsage::ShaderWrite);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a buffer's record keeps the length of its initial data, also for a Gpu buffer", "[core]")
{
    Device device = *createDevice();
    const std::byte bytes[12] = {};
    const Buffer gpu = *device.createBuffer({ .size = 64 }, bytes);
    const Buffer none = *device.createBuffer({ .size = 64 });
    CHECK(core::coreOf(device).pool<Buffer>().lookup(gpu).record->initialBytes == 12);
    CHECK(core::coreOf(device).pool<Buffer>().lookup(none).record->initialBytes == 0);
    destroyDevice(device);
}
