//----------------------------------------------------------------------------------------------------------------------
/// @file FrameArenaCoreTests.cpp
/// @brief Tests which page a frame-arena claim returns, when the arena grows and what rewinding a slot reclaims.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/FrameArena.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using namespace rojoRHI;

namespace {
    constexpr uint64_t kMaxPage = 64 * core::kFrameArenaPageBytes;

    // Numbers each page it creates by its address, 1, 2, 3 and so on, so a test names a page by its creation order.
    struct PageSource
    {
        uint64_t created = 0;

        static Result<core::ArenaPage> create(void* context, uint64_t size)
        {
            PageSource& source = *static_cast<PageSource*>(context);
            return core::ArenaPage{ .size = size, .address = ++source.created };
        }
    };

    uint64_t claim(core::FrameArena& arena, PageSource& source, uint32_t slot, uint64_t size = 64)
    {
        return arena.claim(slot, size, kMaxPage, PageSource::create, &source).address;
    }
} // namespace

TEST_CASE("the lists of a frame claim the slot's pages in creation order after a rewind", "[core]")
{
    core::FrameArena arena(3);
    PageSource source;
    std::vector<uint64_t> first;
    for (uint32_t list = 0; list < 64; ++list)
    {
        first.push_back(claim(arena, source, 1));
    }
    CHECK(arena.growths() == 64);
    CHECK(first.front() == 1);
    CHECK(first.back() == 64);

    arena.rewind(1);
    for (uint32_t list = 0; list < 64; ++list)
    {
        CHECK(claim(arena, source, 1) == first[list]);
    }
    CHECK(arena.growths() == 64);
    // The 65th list of the frame finds every page claimed and grows the slot.
    CHECK(claim(arena, source, 1) == 65);
    CHECK(arena.growths() == 65);
}

TEST_CASE("a frame-arena slot claims and rewinds apart from the others", "[core]")
{
    core::FrameArena arena(3);
    PageSource source;
    CHECK(claim(arena, source, 0) == 1);
    CHECK(claim(arena, source, 2) == 2);
    arena.rewind(0);
    CHECK(claim(arena, source, 2) == 3);
    CHECK(claim(arena, source, 0) == 1);
}

TEST_CASE("a request larger than a standard page passes over the smaller pages, which later claims still find", "[core]")
{
    core::FrameArena arena(1);
    PageSource source;
    const uint64_t large = 3 * core::kFrameArenaPageBytes;
    CHECK(claim(arena, source, 0) == 1);
    CHECK(claim(arena, source, 0, large) == 2);
    CHECK(claim(arena, source, 0) == 3);
    arena.rewind(0);

    // Page 1 is standard, page 2 large, page 3 standard: the large request skips page 1, which stays claimable.
    CHECK(claim(arena, source, 0, large) == 2);
    CHECK(claim(arena, source, 0) == 1);
    CHECK(claim(arena, source, 0) == 3);
    CHECK(claim(arena, source, 0) == 4);
    // No unclaimed page is large enough, so the slot grows by a page of the request's size.
    const core::ArenaPage grown = arena.claim(0, large, kMaxPage, PageSource::create, &source);
    CHECK(grown.address == 5);
    CHECK(grown.size == large);
    CHECK(arena.growths() == 5);
    arena.rewind(0);

    // A growth while standard page 1 is unclaimed leaves it, and the standard pages after it, to the next claims.
    CHECK(claim(arena, source, 0, large) == 2);
    CHECK(claim(arena, source, 0, large) == 5);
    CHECK(claim(arena, source, 0, large) == 6);
    CHECK(claim(arena, source, 0) == 1);
    CHECK(claim(arena, source, 0) == 3);
    CHECK(claim(arena, source, 0) == 4);
    CHECK(claim(arena, source, 0) == 7);
    CHECK(arena.growths() == 7);
}

TEST_CASE("taking a frame arena's pages leaves every slot empty and claimable from its start", "[core]")
{
    core::FrameArena arena(2);
    PageSource source;
    claim(arena, source, 0);
    claim(arena, source, 0);
    claim(arena, source, 1);
    CHECK(arena.takePages().size() == 3);
    CHECK(arena.pageBuffers().empty());
    CHECK(claim(arena, source, 0) == 4);
    CHECK(claim(arena, source, 0) == 5);
}
