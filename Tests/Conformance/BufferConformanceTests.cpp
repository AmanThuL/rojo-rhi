//----------------------------------------------------------------------------------------------------------------------
/// @file BufferConformanceTests.cpp
/// @brief Observes the CPU-visible buffer memory of chapter 03 on a GPU.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

using namespace rojoRHI;

//======================================================================================================================
TEST_CASE("an Upload buffer reads its initial data and later CPU writes back through its CPU pointer", "[conformance]")
{
    test::requireGpu();
    Device device = *createDevice();
    std::array<std::byte, 64> pattern{};
    for (size_t i = 0; i < pattern.size(); ++i)
    {
        pattern[i] = static_cast<std::byte>(i * 3 + 1);
    }
    const Buffer buffer = *device.createBuffer({ .size = 64, .memory = Memory::Upload }, pattern);
    std::byte* pointer = device.cpuPointer(buffer);
    REQUIRE(pointer != nullptr);
    CHECK(std::memcmp(pointer, pattern.data(), pattern.size()) == 0);

    std::array<std::byte, 16> patch{};
    patch.fill(std::byte{ 0xA5 });
    std::memcpy(pointer + 16, patch.data(), patch.size());
    device.beginFrame();
    device.endFrame();
    std::array<std::byte, 64> expected = pattern;
    std::memcpy(expected.data() + 16, patch.data(), patch.size());
    CHECK(device.cpuPointer(buffer) == pointer);
    CHECK(std::memcmp(device.cpuPointer(buffer), expected.data(), expected.size()) == 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Readback buffer's CPU pointer is one stable pointer for the buffer's life", "[conformance]")
{
    test::requireGpu();
    Device device = *createDevice({ .framesInFlight = 2 });
    const Buffer buffer = *device.createBuffer({ .size = 256, .memory = Memory::Readback });
    std::byte* pointer = device.cpuPointer(buffer);
    REQUIRE(pointer != nullptr);
    for (int frame = 0; frame < 6; ++frame)
    {
        device.beginFrame();
        CHECK(device.cpuPointer(buffer) == pointer);
        device.endFrame();
    }
    // The memory stays readable and writable from the CPU for the whole life.
    pointer[0] = std::byte{ 7 };
    pointer[255] = std::byte{ 9 };
    CHECK(device.cpuPointer(buffer)[0] == std::byte{ 7 });
    CHECK(device.cpuPointer(buffer)[255] == std::byte{ 9 });
    destroyDevice(device);
}
