//----------------------------------------------------------------------------------------------------------------------
/// @file SlotAllocatorTests.cpp
/// @brief Tests the slot allocator behind the view heap: order, capacity, owners and deferred reuse.
//----------------------------------------------------------------------------------------------------------------------
#include "Core/SlotAllocator.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <vector>

using namespace rojoRHI::core;

namespace {
    constexpr uint32_t kCapacity = 4;

    /// Collects the native payloads an allocator hands over, in order.
    struct Handed
    {
        std::vector<void*> payloads;

        //==============================================================================================================
        /// Returns the function `retire` and `drain` call with each payload.
        auto sink()
        {
            return [this](void* native) { payloads.push_back(native); };
        }

        //==============================================================================================================
        /// Returns the payloads handed over so far in address order, which for elements of one array is index order.
        std::vector<void*> sorted() const
        {
            std::vector<void*> ordered = payloads;
            std::ranges::sort(ordered, std::less<>{});
            return ordered;
        }
    };
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
    REQUIRE(slots.replace(1, 77, nullptr, [] { return uint64_t{ 1 }; }) == 0);
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
    const auto frame = [] { return uint64_t{ 1 }; };
    CHECK_FALSE(slots.replace(0, 9, nullptr, frame).has_value());
    CHECK_FALSE(slots.replace(7, 9, nullptr, frame).has_value());
    CHECK(slots.replace(2, 9, nullptr, frame) == 0);
    CHECK(slots.replace(2, 10, nullptr, frame) == 9);
    slots.releaseRange(1, 2, frame);
    CHECK_FALSE(slots.replace(2, 11, nullptr, frame).has_value());
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

//======================================================================================================================
TEST_CASE("a released slot hands its native payload over when its frame retires, not before", "[core]")
{
    std::array<int, 1> payload{};
    SlotAllocator slots;
    REQUIRE(slots.allocate(kCapacity, 42, &payload[0]) == 0);
    REQUIRE(slots.release(0, [] { return uint64_t{ 3 }; }) == 42);
    Handed handed;
    slots.retire(2, handed.sink());
    CHECK(handed.payloads.empty());
    slots.retire(3, handed.sink());
    CHECK(handed.payloads == std::vector<void*>{ &payload[0] });
    slots.retire(4, handed.sink());
    CHECK(handed.payloads.size() == 1);
}

//======================================================================================================================
TEST_CASE("a replaced payload waits for the frame the replacement was made in", "[core]")
{
    std::array<int, 2> payload{};
    SlotAllocator slots;
    REQUIRE(slots.allocateRange(kCapacity, 2, 0) == 0);
    Handed handed;
    // The slot held no payload, so the first write queues nothing.
    REQUIRE(slots.replace(0, 5, &payload[0], [] { return uint64_t{ 1 }; }) == 0);
    slots.retire(1, handed.sink());
    CHECK(handed.payloads.empty());
    REQUIRE(slots.replace(0, 6, &payload[1], [] { return uint64_t{ 2 }; }) == 5);
    slots.retire(1, handed.sink());
    CHECK(handed.payloads.empty());
    slots.retire(2, handed.sink());
    CHECK(handed.payloads == std::vector<void*>{ &payload[0] });
    CHECK(slots.state(0) == SlotState::Assigned);
}

//======================================================================================================================
TEST_CASE("a released range hands over the payload of every slot it holds", "[core]")
{
    std::array<int, 3> payload{};
    SlotAllocator slots;
    REQUIRE(slots.allocateRange(kCapacity, 3, 0) == 0);
    const auto frame = [] { return uint64_t{ 1 }; };
    REQUIRE(slots.replace(0, 1, &payload[0], frame).has_value());
    REQUIRE(slots.replace(2, 1, &payload[1], frame).has_value());
    // Replacing slot 0 queues payload 0; the range then releases payload 2 in its place, and each goes over once.
    REQUIRE(slots.replace(0, 1, &payload[2], frame).has_value());
    REQUIRE(slots.releaseRange(0, 3, frame).has_value());
    Handed handed;
    slots.retire(1, handed.sink());
    CHECK(handed.sorted() == std::vector<void*>{ &payload[0], &payload[1], &payload[2] });
    slots.retire(2, handed.sink());
    CHECK(handed.payloads.size() == 3);
}

//======================================================================================================================
TEST_CASE("retire still counts only single slots", "[core]")
{
    std::array<int, 3> payload{};
    SlotAllocator slots;
    const auto frame = [] { return uint64_t{ 1 }; };
    REQUIRE(slots.allocate(8, 1, &payload[0]) == 0);
    REQUIRE(slots.allocateRange(8, 2, 0) == 1);
    REQUIRE(slots.replace(1, 1, &payload[1], frame).has_value());
    REQUIRE(slots.replace(1, 1, &payload[2], frame).has_value());
    REQUIRE(slots.release(0, frame).has_value());
    REQUIRE(slots.releaseRange(1, 2, frame).has_value());
    Handed handed;
    CHECK(slots.retire(1, handed.sink()) == 1);
    CHECK(handed.payloads.size() == 3);
}

//======================================================================================================================
TEST_CASE("drain hands over every held and pending payload once and empties the allocator", "[core]")
{
    std::array<int, 4> payload{};
    SlotAllocator slots;
    const auto frame = [] { return uint64_t{ 1 }; };
    REQUIRE(slots.allocate(8, 1, &payload[0]) == 0);
    REQUIRE(slots.allocate(8, 1, &payload[1]) == 1);
    REQUIRE(slots.allocate(8, 1) == 2);
    REQUIRE(slots.allocateRange(8, 2, 0) == 3);
    REQUIRE(slots.release(1, frame).has_value());
    REQUIRE(slots.replace(3, 1, &payload[2], frame).has_value());
    REQUIRE(slots.replace(3, 1, &payload[3], frame).has_value());
    Handed handed;
    slots.drain(handed.sink());
    CHECK(handed.sorted() == std::vector<void*>{ &payload[0], &payload[1], &payload[2], &payload[3] });
    CHECK(slots.assignedCount() == 0);
    CHECK(slots.state(1) == SlotState::Free);
    slots.retire(1, handed.sink());
    slots.drain(handed.sink());
    CHECK(handed.payloads.size() == 4);
    CHECK(slots.allocate(8, 1) == 0);
}

//======================================================================================================================
TEST_CASE("a reserved leading block is never handed out and capacity counts the slots after it", "[core]")
{
    SlotAllocator slots;
    const auto frame = [] { return uint64_t{ 1 }; };
    slots.reserveLeading(3);
    CHECK(slots.empty());
    CHECK(slots.state(0) == SlotState::Reserved);
    CHECK(slots.state(2) == SlotState::Reserved);
    CHECK(slots.assignedCount() == 0);
    CHECK(slots.allocate(2, 1) == 3);
    CHECK(slots.allocateRange(3, 1, 0) == 4);
    CHECK_FALSE(slots.allocate(2, 1).has_value());
    CHECK_FALSE(slots.allocateRange(3, 2, 0).has_value());
    CHECK(slots.assignedCount() == 2);
    CHECK_FALSE(slots.release(0, frame).has_value());
    CHECK_FALSE(slots.releaseRange(0, 3, frame).has_value());
    CHECK_FALSE(slots.replace(1, 1, nullptr, frame).has_value());
    REQUIRE(slots.release(3, frame).has_value());
    slots.retire(1);
    CHECK(slots.state(0) == SlotState::Reserved);
    CHECK(slots.allocate(2, 1) == 3);
}

//======================================================================================================================
TEST_CASE("drain ends a reservation", "[core]")
{
    SlotAllocator slots;
    slots.reserveLeading(2);
    REQUIRE(slots.allocate(4, 1) == 2);
    Handed handed;
    slots.drain(handed.sink());
    CHECK(slots.empty());
    CHECK(slots.state(0) == SlotState::Free);
    CHECK(slots.allocate(4, 1) == 0);
}
