//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12AddrTests.cpp
/// @brief Tests the D3D12 address lowering: the heap directory, the decode of an Addr and its offset arithmetic.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/Addresses.h"
#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "Core/Records.h"
#include "D3D12Addr.h"
#include "D3D12Device.h"
#include "Support/Death.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>

using namespace rojoRHI;

//======================================================================================================================
TEST_CASE("a published payload is found by its heap index and cleared", "[d3d12]")
{
    d3d12::HeapDirectory directory;
    directory.create(64);
    const d3d12::BufferPayload payload;
    CHECK(directory.find(5) == nullptr);
    directory.publish(5, &payload);
    CHECK(directory.find(5) == &payload);
    CHECK(directory.find(6) == nullptr);
    directory.clear(5);
    CHECK(directory.find(5) == nullptr);
    CHECK(directory.find(64) == nullptr);
}

//======================================================================================================================
TEST_CASE("an index with no entry decodes to slot 0", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    const core::DecodedAddr decoded = core::decodeAddr(DeviceImpl::of(*created).core, Addr{ 12345, 8 });
    CHECK(decoded.slot == 0u);
    CHECK(decoded.offset == 0u);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("an address decodes to its buffer's slot and offset", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    // A handle's low word is its slot index; the generation above it is not part of the decode.
    Buffer buffer;
    buffer.bits = (uint64_t{ 1 } << 32) | 9;
    REQUIRE(core::HandlePool<Buffer, core::BufferRecord>::slotIndex(buffer) == 9u);
    d3d12::BufferPayload payload;
    payload.bufferBits = buffer.bits;
    payload.viewIndex = 40;
    impl.directory.publish(40, &payload);
    const core::DecodedAddr decoded = core::decodeAddr(impl.core, Addr{ 40, 100 });
    CHECK(decoded.slot == 9u);
    CHECK(decoded.offset == 100u);
    impl.directory.clear(40);
    CHECK(core::decodeAddr(impl.core, Addr{ 40, 100 }).slot == 0u);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("operator+ adds to the offset word and keeps the index", "[d3d12]")
{
    const Addr sum = Addr{ 7, 16 } + 48;
    CHECK(sum.lo == 7u);
    CHECK(sum.hi == 64u);
    const Addr last = Addr{ 7, 0 } + (d3d12::kMaxAddrOffset - 1);
    CHECK(last.lo == 7u);
    CHECK(last.hi == d3d12::kMaxAddrOffset - 1);
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0011 operator+ stops at maxAddrOffset", "[d3d12][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0011", [] { (void)(Addr{ 7, 0 } + d3d12::kMaxAddrOffset); });
}
