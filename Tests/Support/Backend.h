//----------------------------------------------------------------------------------------------------------------------
/// @file Backend.h
/// @brief Declares what the tests need from the compiled backend: its library blobs, native surfaces and whether it runs on a GPU.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rojoRHI::test {
    /// The compiled backend's standard library: entries vsMain (vertex), fsMain (fragment), msMain (mesh), tsMain (task) and
    /// csMain (compute), in the backend's library format.
    std::vector<std::byte> standardLibrary();

    /// The standard library's entries stamped with shader ABI version `abi`.
    std::vector<std::byte> libraryStampedWith(uint32_t abi);

    /// A well-formed library of another backend, which the compiled backend must refuse with `DriverFailure`.
    std::vector<std::byte> foreignLibrary();

    /// A native surface of the compiled backend's type, distinct for each `index` below 8 and alive for the process.
    void* surface(uint32_t index = 0);

    /// True when the compiled backend executes work on a GPU, so a conformance case can observe results.
    bool runsOnGpu();
} // namespace rojoRHI::test
