//----------------------------------------------------------------------------------------------------------------------
/// @file HandlePoolTests.cpp
/// @brief Tests the generation-checked handle pool.
//----------------------------------------------------------------------------------------------------------------------
#include "Core/HandlePool.h"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <thread>
#include <vector>

using rojoRHI::Buffer;
using rojoRHI::Texture;
using rojoRHI::core::HandlePool;
using rojoRHI::core::LookupStatus;

namespace {
    struct Rec
    {
        int value = 0;
    };

    using BufferPool = HandlePool<Buffer, Rec>;
} // namespace

//======================================================================================================================
TEST_CASE("a new handle is non-zero and carries the type tag", "[core]")
{
    BufferPool pool;
    const Buffer buffer = pool.allocate({ 7 });
    CHECK(buffer.bits != 0);
    CHECK((buffer.bits >> 56) == rojoRHI::core::handleTypeTag<Buffer>());
    CHECK(rojoRHI::core::handleTypeTag<rojoRHI::Upscaler>() == 7);
}

//======================================================================================================================
TEST_CASE("a lookup returns the stored record", "[core]")
{
    BufferPool pool;
    const Buffer buffer = pool.allocate({ 7 });
    const auto result = pool.lookup(buffer);
    REQUIRE(result.status == LookupStatus::Ok);
    CHECK(result.record->value == 7);
    CHECK(pool.checked(buffer, false) == result.record);
}

//======================================================================================================================
TEST_CASE("the null handle reports core-0001 and is allowed only on request", "[core]")
{
    BufferPool pool;
    CHECK(pool.lookup(Buffer{}).status == LookupStatus::Null);
    CHECK(pool.checked(Buffer{}, true) == nullptr);
}

//======================================================================================================================
TEST_CASE("a released handle is rejected at once as stale", "[core]")
{
    BufferPool pool;
    const Buffer buffer = pool.allocate({ 1 });
    CHECK(pool.release(buffer, 5) == LookupStatus::Ok);
    CHECK(pool.lookup(buffer).status == LookupStatus::StaleGeneration);
    CHECK(pool.release(buffer, 5) == LookupStatus::StaleGeneration);
}

//======================================================================================================================
TEST_CASE("a released slot is reused only after its frame retires", "[core]")
{
    BufferPool pool;
    const Buffer first = pool.allocate({ 1 });
    pool.release(first, 5);

    const Buffer beforeRetire = pool.allocate({ 2 });
    CHECK(static_cast<uint32_t>(beforeRetire.bits) != static_cast<uint32_t>(first.bits));

    pool.retire(4);
    const Buffer stillPending = pool.allocate({ 3 });
    CHECK(static_cast<uint32_t>(stillPending.bits) != static_cast<uint32_t>(first.bits));

    pool.retire(5);
    const Buffer reused = pool.allocate({ 4 });
    CHECK(static_cast<uint32_t>(reused.bits) == static_cast<uint32_t>(first.bits));
    CHECK(reused.bits != first.bits);
    CHECK(pool.lookup(first).status == LookupStatus::StaleGeneration);
    CHECK(pool.lookup(reused).status == LookupStatus::Ok);
}

//======================================================================================================================
TEST_CASE("a handle of another type reports core-0003", "[core]")
{
    BufferPool pool;
    const Buffer buffer = pool.allocate({ 1 });

    HandlePool<Texture, Rec> textures;
    CHECK(textures.lookup(Texture{ buffer.bits }).status == LookupStatus::WrongType);
}

//======================================================================================================================
TEST_CASE("a slot at the last generation is retired for good", "[core]")
{
    HandlePool<Buffer, Rec> pool(2);
    Buffer buffer = pool.allocate({ 1 });
    const uint32_t index = static_cast<uint32_t>(buffer.bits);
    for (int reuse = 0; reuse < 2; ++reuse)
    {
        pool.release(buffer, 1);
        pool.retire(1);
        buffer = pool.allocate({ 1 });
        CHECK(static_cast<uint32_t>(buffer.bits) == index);
    }
    pool.release(buffer, 1);
    pool.retire(1);
    CHECK(pool.lookup(buffer).status == LookupStatus::StaleGeneration);
    CHECK(static_cast<uint32_t>(pool.allocate({ 1 }).bits) != index);
}

//======================================================================================================================
TEST_CASE("the default generation limit is 2^24 - 1", "[core]")
{
    CHECK(BufferPool::kMaxGeneration == 16777215u);
}

//======================================================================================================================
TEST_CASE("concurrent allocate and release keep handles unique and valid", "[core]")
{
    BufferPool pool;
    constexpr int kThreads = 4;
    constexpr int kPerThread = 500;
    std::vector<std::vector<Buffer>> issued(kThreads);
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < kThreads; ++t)
        {
            threads.emplace_back([&, t] {
                for (int i = 0; i < kPerThread; ++i)
                {
                    const Buffer buffer = pool.allocate({ i });
                    issued[t].push_back(buffer);
                    if (i % 2 == 0)
                    {
                        pool.release(buffer, 1);
                    }
                }
            });
        }
    }
    std::set<uint64_t> kept;
    for (const auto& list : issued)
    {
        for (size_t i = 0; i < list.size(); ++i)
        {
            const bool released = i % 2 == 0;
            CHECK((pool.lookup(list[i]).status == LookupStatus::Ok) == !released);
            if (!released)
            {
                kept.insert(list[i].bits);
            }
        }
    }
    CHECK(kept.size() == static_cast<size_t>(kThreads * kPerThread / 2));
    CHECK(pool.liveCount() == kept.size());
}

//======================================================================================================================
TEST_CASE("a released record stays until retire hands it to the callback", "[core]")
{
    BufferPool pool;
    const Buffer buffer = pool.allocate({ 42 });
    pool.release(buffer, 3);

    std::vector<int> seen;
    pool.retire(2, [&](Rec& record) { seen.push_back(record.value); });
    CHECK(seen.empty());
    pool.retire(3, [&](Rec& record) { seen.push_back(record.value); });
    REQUIRE(seen.size() == 1);
    CHECK(seen[0] == 42);
    pool.retire(3, [&](Rec& record) { seen.push_back(record.value); });
    CHECK(seen.size() == 1);
}

//======================================================================================================================
TEST_CASE("a slot retired for good still hands its record to the retire callback", "[core]")
{
    HandlePool<Buffer, Rec> pool(1);
    Buffer buffer = pool.allocate({ 1 });
    pool.release(buffer, 1);
    pool.retire(1);
    buffer = pool.allocate({ 9 });
    pool.release(buffer, 2);
    int seen = 0;
    pool.retire(2, [&](Rec& record) { seen = record.value; });
    CHECK(seen == 9);
}

//======================================================================================================================
TEST_CASE("checkedRelease reads the frame from its supplier under the lock", "[core]")
{
    BufferPool pool;
    const Buffer buffer = pool.allocate({ 7 });
    int calls = 0;
    pool.checkedRelease(buffer, [&] {
        ++calls;
        return uint64_t{ 4 };
    });
    CHECK(calls == 1);

    std::vector<int> seen;
    pool.retire(3, [&](Rec& record) { seen.push_back(record.value); });
    CHECK(seen.empty());
    pool.retire(4, [&](Rec& record) { seen.push_back(record.value); });
    CHECK(seen.size() == 1);
}

//======================================================================================================================
TEST_CASE("drain hands over each live and pending record once and empties the pool", "[core]")
{
    BufferPool pool;
    const Buffer pending = pool.allocate({ 1 });
    const Buffer retired = pool.allocate({ 2 });
    const Buffer live = pool.allocate({ 3 });
    pool.release(retired, 1);
    pool.retire(1);
    pool.release(pending, 2);
    CHECK_FALSE(pool.empty());
    std::multiset<int> seen;
    pool.drain([&](Rec& record) { seen.insert(record.value); });
    CHECK(seen == std::multiset<int>{ 1, 3 });
    CHECK(pool.empty());
    CHECK(pool.lookup(live).status == LookupStatus::StaleGeneration);
    CHECK(pool.lookup(pending).status == LookupStatus::StaleGeneration);
    int again = 0;
    pool.drain([&](Rec&) { ++again; });
    CHECK(again == 0);
}

//======================================================================================================================
TEST_CASE("drain does not hand over a slot already retired for good", "[core]")
{
    HandlePool<Buffer, Rec> pool(1);
    Buffer buffer = pool.allocate({ 1 });
    pool.release(buffer, 1);
    pool.retire(1);
    buffer = pool.allocate({ 2 });
    pool.release(buffer, 2);
    pool.retire(2);
    const Buffer live = pool.allocate({ 3 });
    int seen = 0;
    pool.drain([&](Rec& record) { seen += record.value; });
    CHECK(seen == 3);
    CHECK(pool.lookup(live).status == LookupStatus::StaleGeneration);
}

//======================================================================================================================
TEST_CASE("no slot is reused after drain", "[core]")
{
    BufferPool pool;
    const Buffer buffer = pool.allocate({ 1 });
    pool.release(buffer, 1);
    pool.retire(1); // The slot is now free for reuse.
    pool.drain([](Rec&) {});
    CHECK(static_cast<uint32_t>(pool.allocate({ 2 }).bits) != static_cast<uint32_t>(buffer.bits));
}
