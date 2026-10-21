//----------------------------------------------------------------------------------------------------------------------
/// @file AddressCoreTests.cpp
/// @brief Tests the resolution of an address to its buffer record and byte offset.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/Addresses.h"
#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

using namespace rojoRHI;
using test::PassKit;

//======================================================================================================================
TEST_CASE("an address resolves to its buffer, record and byte offset", "[core]")
{
    PassKit kit;
    const Buffer buffer = kit.buffer(BufferUsage::IndirectArgs, 512);
    core::DeviceCore& device = core::coreOf(kit.device);
    const core::ResolvedAddr at = core::resolveAddr(device, core::coreOf(kit.list), kit.device.address(buffer, 40));
    CHECK(at.buffer.bits == buffer.bits);
    CHECK(at.offset == 40);
    REQUIRE(at.record != nullptr);
    CHECK(at.record->size == 512);
    kit.finish();
}

//======================================================================================================================
TEST_CASE("the null address resolves to no buffer and issues no rule", "[core]")
{
    PassKit kit;
    const core::ResolvedAddr at = core::resolveAddr(core::coreOf(kit.device), core::coreOf(kit.list), Addr{});
    CHECK(at.buffer.bits == 0);
    CHECK(at.record == nullptr);
    kit.finish();
}

//======================================================================================================================
TEST_CASE("frame data resolves on a list of the frame that produced it", "[core]")
{
    PassKit kit;
    const std::array<std::byte, 8> bytes = {};
    const Addr data = kit.list.uploadFrameData(bytes, 256);
    const core::ResolvedAddr at = core::resolveAddr(core::coreOf(kit.device), core::coreOf(kit.list), data);
    REQUIRE(at.record != nullptr);
    CHECK(at.offset == 0);
    CHECK(at.record->label == "rojorhi.frameArena.page");
    kit.finish();
}

//======================================================================================================================
TEST_CASE("an arena page's record holds the frame of the list that claimed it in Debug builds", "[core]")
{
    PassKit kit;
    const std::array<std::byte, 8> bytes = {};
    (void)kit.list.uploadFrameData(bytes);
    core::DeviceCore& device = core::coreOf(kit.device);
    const Buffer page = device.arena().pageBuffers().front();
#ifndef NDEBUG
    const uint64_t claimed = 1;
#else
    // Only Debug-tier code reads the frame (VU-rojo-buffer-0020 and the tracker's references), so a Release claim writes none.
    const uint64_t claimed = 0;
#endif
    CHECK(device.pool<Buffer>().lookup(page).record->arenaFrame == claimed);
    CHECK(device.pool<Buffer>().lookup(kit.buffer(BufferUsage::None)).record->arenaFrame == 0);
    kit.finish();
}
