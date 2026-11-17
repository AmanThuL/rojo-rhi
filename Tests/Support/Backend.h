//----------------------------------------------------------------------------------------------------------------------
/// @file Backend.h
/// @brief Declares what the tests need from the compiled backend: its library blobs, native surfaces and whether it runs on a GPU.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace rojoRHI::test {
    // Entries vsMain, fsMain, msMain, tsMain and csMain, read from the packaged `Shaders/Standard` file beside the test binary
    // in the backend's library format (`Standard.nulllib` on Null).
    std::vector<std::byte> standardLibrary();

    std::vector<std::byte> libraryStampedWith(uint32_t abi);

    // Well-formed for another backend; the compiled backend must refuse it with `DriverFailure`.
    std::vector<std::byte> foreignLibrary();

    // `name` is a file of `Shaders/Tests/` without its extension, read in the compiled backend's format from beside the test
    // binary. Fails the running case when the file is missing or empty.
    std::vector<std::byte> shaderLibraryFile(std::string_view name);

    // Distinct for each `index` below 8 and alive for the process.
    void* surface(uint32_t index = 0);

    bool runsOnGpu();

    // True on the pinned Slang's Metal lowering, whose conformance shaders read a multisample view's size, not a sample.
    bool multisampleReadsSize();

    // Why the runtime ends the process at the clear of a texture that an earlier barrier in the same list moved to a layout
    // other than `CopyDst`, before the tracker judges the list at submission; empty when it lets the tracker judge it. A
    // death case that records such a clear skips with the reason.
    std::string_view misplacedClearEndsRecording();

    // True when layouts lower to native ones the runtime checks, so a texture must leave `Undefined` through a barrier
    // before a pass or a clear uses it, even on a device that does not validate.
    bool layoutsAreNative();

    // Why the device never completes a submitted upscale, or empty when it completes one: a case that submits an upscale
    // skips with the reason.
    std::string_view upscalesNeverComplete();

    // Why the device is known to fuse a precise file's multiply and add, or empty when it is not: the precision case then
    // accepts the fused result as well as the exact one, and skips with the reason on the fused one.
    std::string_view fusesPreciseArithmetic();
} // namespace rojoRHI::test
