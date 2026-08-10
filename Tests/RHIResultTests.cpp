#include "RHI/Result.h"

#include <catch2/catch_test_macros.hpp>

#include <expected>
#include <type_traits>

using namespace lmx::rhi;

static_assert(std::is_same_v<Result<int>, std::expected<int, Error>>);
static_assert(std::is_same_v<Result<void>, std::expected<void, Error>>);

//======================================================================================================================
TEST_CASE("RHI Result carries values and domain errors", "[unit][rhi]") {
    Result<int> value = 42;
    REQUIRE(value.has_value());
    REQUIRE(*value == 42);

    Result<int> failure = std::unexpected(Error{ErrorCode::InvalidDesc, "invalid descriptor"});
    REQUIRE_FALSE(failure.has_value());
    REQUIRE(failure.error().code == ErrorCode::InvalidDesc);
    REQUIRE(failure.error().message == "invalid descriptor");
}
