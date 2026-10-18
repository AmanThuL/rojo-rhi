//----------------------------------------------------------------------------------------------------------------------
/// @file Backend.h
/// @brief Declares what the tests need from the compiled backend: its library blob, its native surfaces and whether it runs on a GPU.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rojoRHI::test {
    /// The blob of the standard library: entries vsMain (vertex), fsMain (fragment), msMain (mesh), tsMain (task) and csMain
    /// (compute), in the compiled backend's library format.
    std::vector<std::byte> standardLibrary();

    /// A native surface of the compiled backend's type, distinct for each `index` below 8 and alive for the process.
    void* surface(uint32_t index = 0);

    /// True when the compiled backend executes work on a GPU, so a conformance case can observe results.
    bool runsOnGpu();
} // namespace rojoRHI::test
