//----------------------------------------------------------------------------------------------------------------------
/// @file BufferTests.cpp
/// @brief Tests buffer creation, addresses and CPU pointers against chapter 03.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <type_traits>

using namespace rojoRHI;

namespace {
    static_assert(sizeof(Addr) == 8 && alignof(Addr) == 8);
    static_assert(std::is_trivially_copyable_v<Addr>);

    //==================================================================================================================
    Device makeDevice()
    {
        Result<Device> created = createDevice();
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    Buffer makeBuffer(Device device, BufferDesc desc, std::span<const std::byte> initial = {})
    {
        Result<Buffer> created = device.createBuffer(desc, initial);
        REQUIRE(created.has_value());
        return *created;
    }
} // namespace

//======================================================================================================================
TEST_CASE("a buffer is created in every memory class", "[contract]")
{
    Device device = makeDevice();
    for (const Memory memory : { Memory::Gpu, Memory::Upload, Memory::Readback })
    {
        const Buffer buffer = makeBuffer(device, { .size = 256, .memory = memory });
        CHECK(buffer != Buffer{});
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("two buffers have different handles", "[contract]")
{
    Device device = makeDevice();
    CHECK(makeBuffer(device, { .size = 16 }) != makeBuffer(device, { .size = 16 }));
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("usage flags combine with operator| and test with operator&", "[contract]")
{
    constexpr BufferUsage both = BufferUsage::Index | BufferUsage::IndirectArgs;
    static_assert((both & BufferUsage::Index) == BufferUsage::Index);
    static_assert((both & BufferUsage::ShaderWrite) == BufferUsage::None);
    SUCCEED();
}

//======================================================================================================================
TEST_CASE("a Gpu buffer takes ShaderWrite and Index and IndirectArgs", "[contract]")
{
    Device device = makeDevice();
    const Buffer buffer = makeBuffer(device, { .size = 64, .usage = BufferUsage::ShaderWrite | BufferUsage::Index | BufferUsage::IndirectArgs });
    CHECK(buffer != Buffer{});
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an Upload buffer takes Index and IndirectArgs", "[contract]")
{
    Device device = makeDevice();
    const Buffer buffer = makeBuffer(device, { .size = 64, .memory = Memory::Upload, .usage = BufferUsage::Index | BufferUsage::IndirectArgs });
    CHECK(buffer != Buffer{});
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Sparse buffer is created when the device reports sparse resources", "[contract]")
{
    Device device = makeDevice();
    if (device.capabilities().placementSparse)
    {
        CHECK(makeBuffer(device, { .size = 1 << 20, .usage = BufferUsage::Sparse }) != Buffer{});
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("initial data is visible through the CPU pointer of an Upload buffer", "[contract]")
{
    Device device = makeDevice();
    const std::array<std::byte, 4> bytes = { std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 }, std::byte{ 4 } };
    const Buffer buffer = makeBuffer(device, { .size = 64, .memory = Memory::Upload }, bytes);
    const std::byte* pointer = device.cpuPointer(buffer);
    REQUIRE(pointer != nullptr);
    CHECK(std::memcmp(pointer, bytes.data(), bytes.size()) == 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("initial data of exactly the buffer size is accepted", "[contract]")
{
    Device device = makeDevice();
    const std::array<std::byte, 8> bytes{};
    CHECK(makeBuffer(device, { .size = bytes.size() }, bytes) != Buffer{});
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the CPU pointer of an Upload buffer is writable and stable", "[contract]")
{
    Device device = makeDevice();
    const Buffer buffer = makeBuffer(device, { .size = 64, .memory = Memory::Upload });
    std::byte* pointer = device.cpuPointer(buffer);
    REQUIRE(pointer != nullptr);
    pointer[63] = std::byte{ 9 };
    CHECK(device.cpuPointer(buffer) == pointer);
    CHECK(device.cpuPointer(buffer)[63] == std::byte{ 9 });
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Readback buffer has a CPU pointer", "[contract]")
{
    Device device = makeDevice();
    CHECK(device.cpuPointer(makeBuffer(device, { .size = 64, .memory = Memory::Readback })) != nullptr);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the address of a buffer is not null and is stable", "[contract]")
{
    Device device = makeDevice();
    const Buffer buffer = makeBuffer(device, { .size = 256 });
    const Addr first = device.address(buffer);
    const Addr second = device.address(buffer);
    CHECK((first.lo != 0 || first.hi != 0));
    CHECK(std::memcmp(&first, &second, sizeof(Addr)) == 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the address of byte N equals the base address plus N", "[contract]")
{
    Device device = makeDevice();
    const Buffer buffer = makeBuffer(device, { .size = 256 });
    const Addr expected = device.address(buffer) + 100;
    const Addr actual = device.address(buffer, 100);
    CHECK(std::memcmp(&expected, &actual, sizeof(Addr)) == 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the address of the last byte is accepted", "[contract]")
{
    Device device = makeDevice();
    const Buffer buffer = makeBuffer(device, { .size = 256 });
    CHECK((device.address(buffer, 255).lo != 0 || device.address(buffer, 255).hi != 0));
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("two buffers have different addresses", "[contract]")
{
    Device device = makeDevice();
    const Addr a = device.address(makeBuffer(device, { .size = 256 }));
    const Addr b = device.address(makeBuffer(device, { .size = 256 }));
    CHECK(std::memcmp(&a, &b, sizeof(Addr)) != 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an Upload buffer has an address", "[contract]")
{
    Device device = makeDevice();
    const Addr address = device.address(makeBuffer(device, { .size = 256, .memory = Memory::Upload }));
    CHECK((address.lo != 0 || address.hi != 0));
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the null Addr is both words zero", "[contract]")
{
    constexpr Addr none{};
    CHECK(none.lo == 0);
    CHECK(none.hi == 0);
}

//======================================================================================================================
TEST_CASE("adding zero leaves an address unchanged", "[contract]")
{
    Device device = makeDevice();
    const Addr base = device.address(makeBuffer(device, { .size = 256 }));
    const Addr same = base + 0;
    CHECK(std::memcmp(&base, &same, sizeof(Addr)) == 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a destroyed buffer's replacement gets a fresh handle", "[contract]")
{
    Device device = makeDevice();
    const Buffer old = makeBuffer(device, { .size = 64 });
    device.destroy(old);
    CHECK(makeBuffer(device, { .size = 64 }) != old);
    destroyDevice(device);
}
