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
    /// The compiled backend's standard library: entries vsMain (vertex), fsMain (fragment), msMain (mesh), tsMain (task) and
    /// csMain (compute), in the backend's library format.
    std::vector<std::byte> standardLibrary();

    /// The standard library's entries stamped with shader ABI version `abi`.
    std::vector<std::byte> libraryStampedWith(uint32_t abi);

    /// A well-formed library of another backend, which the compiled backend must refuse with `DriverFailure`.
    std::vector<std::byte> foreignLibrary();

    /// The conformance shader library `name` (a file of `Shaders/Tests/` without its extension) in the compiled backend's
    /// format, read from the `Shaders/` directory beside the test binary. Fails the running case when the backend packages no
    /// such file: the Null backend never does, so a case calls it only after `requireGpu()` has passed.
    std::vector<std::byte> shaderLibraryFile(std::string_view name);

    /// A native surface of the compiled backend's type, distinct for each `index` below 8 and alive for the process.
    void* surface(uint32_t index = 0);

    /// True when the compiled backend executes work on a GPU, so a conformance case can observe results.
    bool runsOnGpu();

    /// True when the compiled backend's conformance shaders read a multisample view's size, not a sample: the pinned Slang's Metal lowering.
    bool multisampleReadsSize();

    /// Why the compiled backend's runtime ends the process at the clear of a texture that a barrier earlier in the same list
    /// moved to a layout other than `CopyDst`, before the tracker judges the list at submission, or empty when it lets the
    /// tracker judge it: a death case that records such a clear skips with the reason.
    std::string_view misplacedClearEndsRecording();

    /// True when the compiled backend lowers layouts to native ones its runtime checks, so a texture must leave `Undefined`
    /// through a barrier before a pass or a clear uses it even on a device that does not validate.
    bool layoutsAreNative();
} // namespace rojoRHI::test
