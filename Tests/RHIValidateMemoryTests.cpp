#include "RHIValidateTestSupport.h"

//======================================================================================================================
TEST_CASE("indirect arguments at an aligned in-bounds offset are accepted", "[rhi]") {
    const FakeBuffer buffer{256};

    REQUIRE(validateIndirectArgs(buffer, 64, sizeof(DrawIndexedIndirectArgs)).has_value());
}

//======================================================================================================================
TEST_CASE("indirect arguments at an unaligned offset are rejected", "[rhi]") {
    const FakeBuffer buffer{256};

    const auto r = validateIndirectArgs(buffer, 6, sizeof(DispatchIndirectArgs));
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().code == ErrorCode::InvalidDesc);
    REQUIRE(r.error().message.contains("multiple of 4"));
}

//======================================================================================================================
// The whole struct has to fit: an offset four bytes short of the end is in bounds by itself and
// still reads past the allocation.
TEST_CASE("indirect arguments running past the buffer are rejected", "[rhi]") {
    const FakeBuffer buffer{16};

    const auto r = validateIndirectArgs(buffer, 8, sizeof(DrawIndirectArgs));
    REQUIRE_FALSE(r.has_value());
    REQUIRE(r.error().message.contains("past the buffer's"));
}

//======================================================================================================================
// The shape a caller must get right before any frame-owned memory is touched. Capacity is
// deliberately absent: the arena grows, so running out of it is the backend's failure to report,
// not a request the caller could have made smaller.
TEST_CASE("a frame-data request is checked for slot, block, and alignment", "[rhi][validate]") {
    int block = 0;

    REQUIRE(validateFrameData(0, &block, sizeof(block), kFrameDataAlignment).has_value());
    REQUIRE(validateFrameData(CommandList::kMaxBufferBindings - 1, &block, 1, 4096).has_value());

    const auto slot = validateFrameData(CommandList::kMaxBufferBindings, &block, 4, 256);
    REQUIRE_FALSE(slot.has_value());
    REQUIRE(slot.error().code == ErrorCode::InvalidDesc);
    REQUIRE(slot.error().message.contains("buffer binding count"));

    REQUIRE_FALSE(validateFrameData(0, nullptr, 4, 256).has_value());
    REQUIRE_FALSE(validateFrameData(0, &block, 0, 256).has_value());
}

//======================================================================================================================
// Both halves of the alignment rule, each with its own diagnostic: too small breaks the placement
// contract a constant buffer is read through, and not a power of two is not an alignment at all.
TEST_CASE("a frame-data alignment must be a power of two of at least 256", "[rhi][validate]") {
    int block = 0;

    REQUIRE(validateFrameData(0, &block, 4, 256).has_value());
    REQUIRE(validateFrameData(0, &block, 4, 512).has_value());
    REQUIRE(validateFrameData(0, &block, 4, 65536).has_value());

    const auto small = validateFrameData(0, &block, 4, 128);
    REQUIRE_FALSE(small.has_value());
    REQUIRE(small.error().message.contains("at least 256"));

    const auto odd = validateFrameData(0, &block, 4, 384);
    REQUIRE_FALSE(odd.has_value());
    REQUIRE(odd.error().message.contains("power of two"));

    REQUIRE_FALSE(validateFrameData(0, &block, 4, 0).has_value());
}

//======================================================================================================================
// A heap with no bytes is a heap nothing can be placed in, which is a caller error rather than an
// empty success.
TEST_CASE("HeapDesc requires a size", "[rhi][validate]") {
    REQUIRE_FALSE(validate(HeapDesc{.size = 0}).has_value());
    REQUIRE(validate(HeapDesc{.size = 4096, .label = "rojorhi.test.heap"}).has_value());
}

//======================================================================================================================
// The three ways a placement can be wrong, each caught before the driver sees it: a resource the
// backend declined to size, an offset the resource's alignment does not divide, and a footprint
// running off the end of the heap. The last is checked at the exact boundary from both sides,
// because that is where an off-by-one lives.
TEST_CASE("a placement is checked against its heap and its alignment", "[rhi][validate]") {
    const FakeHeap heap{4096};

    REQUIRE_FALSE(validatePlacement(heap, 0, {.size = 0, .alignment = 256}).has_value());
    REQUIRE_FALSE(validatePlacement(heap, 0, {.size = 256, .alignment = 0}).has_value());

    REQUIRE_FALSE(validatePlacement(heap, 128, {.size = 256, .alignment = 256}).has_value());
    REQUIRE(validatePlacement(heap, 256, {.size = 256, .alignment = 256}).has_value());

    // Exactly filling the heap is legal; one byte of size past it is not.
    REQUIRE(validatePlacement(heap, 3840, {.size = 256, .alignment = 256}).has_value());
    REQUIRE_FALSE(validatePlacement(heap, 3840, {.size = 512, .alignment = 256}).has_value());
    REQUIRE_FALSE(validatePlacement(heap, 4096, {.size = 256, .alignment = 256}).has_value());
}
