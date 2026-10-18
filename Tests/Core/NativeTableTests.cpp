//----------------------------------------------------------------------------------------------------------------------
/// @file NativeTableTests.cpp
/// @brief Tests the lock-free table of native payloads a handle pool keeps per slot.
//----------------------------------------------------------------------------------------------------------------------
#include "Core/NativeTable.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <thread>

using rojoRHI::core::NativeTable;

//======================================================================================================================
TEST_CASE("a published payload loads back from its slot", "[core]")
{
    NativeTable table;
    int payload = 0;
    table.publish(7, &payload);
    CHECK(table.load(7) == &payload);
    table.publish(7, nullptr);
    CHECK(table.load(7) == nullptr);
}

//======================================================================================================================
TEST_CASE("an unpublished slot and a slot of an absent chunk load null", "[core]")
{
    NativeTable table;
    int payload = 0;
    table.publish(1, &payload);
    CHECK(table.load(2) == nullptr);
    CHECK(table.load(NativeTable::kChunkSlots * 3 + 1) == nullptr);
    CHECK(table.load(NativeTable::kCapacity - 1) == nullptr);
}

//======================================================================================================================
TEST_CASE("slots in two chunks keep separate payloads", "[core]")
{
    NativeTable table;
    int first = 0;
    int second = 0;
    table.publish(5, &first);
    table.publish(NativeTable::kChunkSlots + 5, &second);
    CHECK(table.load(5) == &first);
    CHECK(table.load(NativeTable::kChunkSlots + 5) == &second);
}

//======================================================================================================================
TEST_CASE("a reader on another thread sees a payload published before the handle reached it", "[core]")
{
    NativeTable table;
    int payload = 0;
    std::atomic<uint32_t> handed{ 0 };
    void* seen = nullptr;
    std::jthread reader([&] {
        uint32_t index = 0;
        while ((index = handed.load(std::memory_order_acquire)) == 0)
        {
            std::this_thread::yield();
        }
        seen = table.load(index);
    });
    table.publish(NativeTable::kChunkSlots * 2 + 9, &payload);
    handed.store(NativeTable::kChunkSlots * 2 + 9, std::memory_order_release);
    reader.join();
    CHECK(seen == &payload);
}

//======================================================================================================================
TEST_CASE("a table reports the capacity it was created with", "[core]")
{
    CHECK(NativeTable::kCapacity == (1u << 22));
    CHECK(NativeTable().capacity() == NativeTable::kCapacity);
    CHECK(NativeTable(3).capacity() == 3);
}

//======================================================================================================================
TEST_CASE("a load past the capacity stays inside the directory", "[core]")
{
    NativeTable table;
    int payload = 0;
    table.publish(3, &payload);
    CHECK(table.load(NativeTable::kCapacity + 3) == &payload);
    CHECK(table.load(UINT32_MAX) == nullptr);
}
