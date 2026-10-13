//----------------------------------------------------------------------------------------------------------------------
/// @file SlotAllocatorTests.cpp
/// @brief Tests the slot allocator behind the view heap: order, capacity, owners and deferred reuse.
//----------------------------------------------------------------------------------------------------------------------
#include "Core/SlotAllocator.h"

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

using namespace rojoRHI::core;

namespace {
    constexpr uint32_t kCapacity = 4;
} // namespace

//======================================================================================================================
TEST_CASE("fresh slots are handed out from zero in order", "[core]")
{
    SlotAllocator slots;
    CHECK(slots.allocate(kCapacity, 10) == 0);
    CHECK(slots.allocate(kCapacity, 11) == 1);
    CHECK(slots.assignedCount() == 2);
    CHECK(slots.state(1) == SlotState::Assigned);
    CHECK(slots.state(2) == SlotState::Free);
}

//======================================================================================================================
TEST_CASE("a full heap hands out nothing", "[core]")
{
    SlotAllocator slots;
    for (uint32_t i = 0; i < kCapacity; ++i)
    {
        REQUIRE(slots.allocate(kCapacity, i).has_value());
    }
    CHECK_FALSE(slots.allocate(kCapacity, 99).has_value());
}

//======================================================================================================================
TEST_CASE("release returns the owner and a released slot is not reused before its frame retires", "[core]")
{
    SlotAllocator slots;
    REQUIRE(slots.allocate(kCapacity, 42) == 0);
    CHECK(slots.release(0, [] { return uint64_t{ 5 }; }) == 42);
    CHECK(slots.state(0) == SlotState::Released);
    CHECK(slots.allocate(kCapacity, 1) == 1);
    slots.retire(4);
    CHECK(slots.state(0) == SlotState::Released);
    CHECK(slots.allocate(kCapacity, 2) == 2);
    slots.retire(5);
    CHECK(slots.state(0) == SlotState::Free);
    CHECK(slots.allocate(kCapacity, 3) == 0);
}

//======================================================================================================================
TEST_CASE("a retired slot is reused even when the heap is full", "[core]")
{
    SlotAllocator slots;
    for (uint32_t i = 0; i < kCapacity; ++i)
    {
        REQUIRE(slots.allocate(kCapacity, i).has_value());
    }
    slots.release(2, [] { return uint64_t{ 1 }; });
    CHECK_FALSE(slots.allocate(kCapacity, 7).has_value());
    slots.retire(1);
    CHECK(slots.allocate(kCapacity, 7) == 2);
}

//======================================================================================================================
TEST_CASE("releasing a slot that is not assigned releases nothing", "[core]")
{
    SlotAllocator slots;
    CHECK_FALSE(slots.release(0, [] { return uint64_t{ 1 }; }).has_value());
    REQUIRE(slots.allocate(kCapacity, 1) == 0);
    REQUIRE(slots.release(0, [] { return uint64_t{ 1 }; }).has_value());
    CHECK_FALSE(slots.release(0, [] { return uint64_t{ 1 }; }).has_value());
    CHECK_FALSE(slots.release(3, [] { return uint64_t{ 1 }; }).has_value());
}

//======================================================================================================================
TEST_CASE("a range takes contiguous slots after the singles and is not split by a single", "[core]")
{
    SlotAllocator slots;
    REQUIRE(slots.allocate(8, 1) == 0);
    CHECK(slots.allocateRange(8, 3, 0) == 1);
    CHECK(slots.allocate(8, 2) == 4);
    CHECK(slots.assignedCount() == 5);
}

//======================================================================================================================
TEST_CASE("a range needs one contiguous run, not just enough free slots", "[core]")
{
    SlotAllocator slots;
    for (uint32_t i = 0; i < 6; ++i)
    {
        REQUIRE(slots.allocate(6, i) == i);
    }
    slots.release(1, [] { return uint64_t{ 1 }; });
    slots.release(3, [] { return uint64_t{ 1 }; });
    slots.retire(1);
    CHECK_FALSE(slots.allocateRange(6, 2, 0).has_value());
    CHECK(slots.allocateRange(6, 1, 0).has_value());
}

//======================================================================================================================
TEST_CASE("a range reuses a retired run and leaves its slots out of the single free list", "[core]")
{
    SlotAllocator slots;
    for (uint32_t i = 0; i < 6; ++i)
    {
        REQUIRE(slots.allocate(6, i) == i);
    }
    for (uint32_t i = 2; i < 5; ++i)
    {
        slots.release(i, [] { return uint64_t{ 1 }; });
    }
    slots.retire(1);
    CHECK(slots.allocateRange(6, 3, 0) == 2);
    CHECK_FALSE(slots.allocate(6, 9).has_value());
}

//======================================================================================================================
TEST_CASE("a range can run from retired slots into never-used ones", "[core]")
{
    SlotAllocator slots;
    REQUIRE(slots.allocate(5, 0) == 0);
    REQUIRE(slots.allocate(5, 0) == 1);
    slots.release(1, [] { return uint64_t{ 1 }; });
    slots.retire(1);
    CHECK(slots.allocateRange(5, 4, 0) == 1);
    CHECK_FALSE(slots.allocateRange(5, 1, 0).has_value());
}

//======================================================================================================================
TEST_CASE("a range larger than the capacity is refused", "[core]")
{
    SlotAllocator slots;
    CHECK_FALSE(slots.allocateRange(4, 5, 0).has_value());
    CHECK(slots.allocateRange(4, 4, 0) == 0);
}

//======================================================================================================================
TEST_CASE("a released range returns its owners and is not reused before its frame retires", "[core]")
{
    SlotAllocator slots;
    REQUIRE(slots.allocateRange(4, 3, 0) == 0);
    REQUIRE(slots.replaceOwner(1, 77) == 0);
    const std::optional<std::vector<uint64_t>> owners = slots.releaseRange(0, 3, [] { return uint64_t{ 2 }; });
    REQUIRE(owners.has_value());
    CHECK(*owners == std::vector<uint64_t>{ 0, 77, 0 });
    CHECK_FALSE(slots.allocateRange(4, 3, 0).has_value());
    slots.retire(1);
    CHECK_FALSE(slots.allocateRange(4, 3, 0).has_value());
    slots.retire(2);
    CHECK(slots.allocateRange(4, 3, 0) == 0);
}

//======================================================================================================================
TEST_CASE("only the exact range that was reserved can be released, once", "[core]")
{
    SlotAllocator slots;
    REQUIRE(slots.allocateRange(8, 3, 0) == 0);
    const auto frame = [] { return uint64_t{ 1 }; };
    CHECK_FALSE(slots.releaseRange(0, 2, frame).has_value());
    CHECK_FALSE(slots.releaseRange(1, 2, frame).has_value());
    CHECK_FALSE(slots.releaseRange(0, 0, frame).has_value());
    CHECK(slots.releaseRange(0, 3, frame).has_value());
    CHECK_FALSE(slots.releaseRange(0, 3, frame).has_value());
}

//======================================================================================================================
TEST_CASE("a slot of a range is not released as a single and a single is not released as a range", "[core]")
{
    SlotAllocator slots;
    REQUIRE(slots.allocateRange(8, 2, 0) == 0);
    REQUIRE(slots.allocate(8, 5) == 2);
    const auto frame = [] { return uint64_t{ 1 }; };
    CHECK_FALSE(slots.release(0, frame).has_value());
    CHECK_FALSE(slots.release(1, frame).has_value());
    CHECK_FALSE(slots.releaseRange(2, 1, frame).has_value());
    CHECK(slots.release(2, frame) == 5);
}

//======================================================================================================================
TEST_CASE("an owner is replaced only in a slot of an assigned range", "[core]")
{
    SlotAllocator slots;
    REQUIRE(slots.allocate(8, 5) == 0);
    REQUIRE(slots.allocateRange(8, 2, 0) == 1);
    CHECK_FALSE(slots.replaceOwner(0, 9).has_value());
    CHECK_FALSE(slots.replaceOwner(7, 9).has_value());
    CHECK(slots.replaceOwner(2, 9) == 0);
    CHECK(slots.replaceOwner(2, 10) == 9);
    slots.releaseRange(1, 2, [] { return uint64_t{ 1 }; });
    CHECK_FALSE(slots.replaceOwner(2, 11).has_value());
}

//======================================================================================================================
TEST_CASE("holdsRange is true only for the exact assigned range", "[core]")
{
    SlotAllocator slots;
    REQUIRE(slots.allocate(8, 1) == 0);
    REQUIRE(slots.allocateRange(8, 3, 0) == 1);
    CHECK(slots.holdsRange(1, 3));
    CHECK_FALSE(slots.holdsRange(1, 2));
    CHECK_FALSE(slots.holdsRange(2, 2));
    CHECK_FALSE(slots.holdsRange(0, 1));
    CHECK_FALSE(slots.holdsRange(1, 0));
    slots.releaseRange(1, 3, [] { return uint64_t{ 1 }; });
    CHECK_FALSE(slots.holdsRange(1, 3));
}
