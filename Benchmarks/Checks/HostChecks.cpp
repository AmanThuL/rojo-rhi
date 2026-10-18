//----------------------------------------------------------------------------------------------------------------------
/// @file HostChecks.cpp
/// @brief Checks the host services every measurement uses: allocation counter, clock, thermal state and environment.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/JsonWriter.h"
#include "Host/AllocationCounter.h"
#include "Host/Environment.h"
#include "Host/HostClock.h"
#include "Host/ThermalState.h"

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>
#include <catch2/catch_test_macros.hpp>
#include <malloc/malloc.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

using namespace rojoRHI::bench;

namespace {
    //==================================================================================================================
    // The keys of the top-level object of a JSON text, read by tracking string and nesting state.
    std::set<std::string> topLevelKeys(std::string_view text)
    {
        std::set<std::string> keys;
        int depth = 0;
        size_t index = 0;
        while (index < text.size())
        {
            const char c = text[index];
            if (c == '"')
            {
                std::string value;
                ++index;
                while (index < text.size() && text[index] != '"')
                {
                    if (text[index] == '\\')
                    {
                        ++index;
                    }
                    value += text[index];
                    ++index;
                }
                ++index;
                size_t next = index;
                while (next < text.size() && (text[next] == ' ' || text[next] == '\n'))
                {
                    ++next;
                }
                if (depth == 1 && next < text.size() && text[next] == ':')
                {
                    keys.insert(value);
                }
                continue;
            }
            if (c == '{' || c == '[')
            {
                ++depth;
            }
            else if (c == '}' || c == ']')
            {
                --depth;
            }
            ++index;
        }
        return keys;
    }
} // namespace

//======================================================================================================================
TEST_CASE("the allocation counter passes the self-check it runs at installation", "[host]")
{
    // Installation allocates one block and fails unless the count rose by exactly one, as it does not when malloc
    // dispatches to another zone (MallocStackLogging, for one).
    const auto installed = installAllocationCounter();
    INFO((installed ? std::string("installed") : installed.error()));
    CHECK(installed.has_value());
}

//======================================================================================================================
TEST_CASE("the allocation counter counts each allocation call once", "[host]")
{
    REQUIRE(installAllocationCounter().has_value());
    // Each result is stored through a volatile pointer, so the compiler cannot elide the allocation.
    uint64_t before = allocationCount();
    void* volatile block = std::malloc(100);
    const uint64_t afterMalloc = allocationCount() - before;

    before = allocationCount();
    void* volatile zeroed = std::calloc(3, 40);
    const uint64_t afterCalloc = allocationCount() - before;

    before = allocationCount();
    block = std::realloc(block, 100'000);
    const uint64_t afterRealloc = allocationCount() - before;

    before = allocationCount();
    void* aligned = nullptr;
    const int alignedResult = posix_memalign(&aligned, 64, 128);
    void* volatile alignedBlock = aligned;
    const uint64_t afterMemalign = allocationCount() - before;

    before = allocationCount();
    int* volatile object = new int(3);
    const uint64_t afterNew = allocationCount() - before;

    before = allocationCount();
    void* volatile paged = valloc(100);
    const uint64_t afterValloc = allocationCount() - before;

    before = allocationCount();
    void* volatile zoneAligned = malloc_zone_memalign(malloc_default_zone(), 64, 256);
    const uint64_t afterZoneMemalign = allocationCount() - before;

    // The malloc_with_options entry exists from macOS 26.
    void* volatile optioned = nullptr;
    uint64_t afterOptions = 1;
    if (__builtin_available(macOS 26.0, *))
    {
        before = allocationCount();
        optioned = malloc_zone_malloc_with_options(nullptr, 64, 256, MALLOC_ZONE_MALLOC_OPTION_CLEAR);
        afterOptions = allocationCount() - before;
    }

    before = allocationCount();
    std::free(block);
    const uint64_t afterFree = allocationCount() - before;

    std::array<void*, 4> batch{};
    before = allocationCount();
    const unsigned batched = malloc_zone_batch_malloc(malloc_default_zone(), 32, batch.data(), static_cast<unsigned>(batch.size()));
    const uint64_t afterBatch = allocationCount() - before;

    malloc_zone_batch_free(malloc_default_zone(), batch.data(), batched);
    delete object;
    std::free(optioned);
    std::free(zoneAligned);
    std::free(paged);
    std::free(alignedBlock);
    std::free(zeroed);

    REQUIRE(alignedResult == 0);
    CHECK(afterMalloc == 1);
    CHECK(afterCalloc == 1);
    CHECK(afterRealloc == 1);
    CHECK(afterMemalign == 1);
    CHECK(afterNew == 1);
    CHECK(afterValloc == 1);
    CHECK(afterZoneMemalign == 1);
    CHECK(afterOptions == 1);
    CHECK(afterFree == 0);
    CHECK(afterBatch == batched);
}

//======================================================================================================================
TEST_CASE("the allocation counter sees allocations of other threads", "[host]")
{
    REQUIRE(installAllocationCounter().has_value());
    constexpr uint64_t kAllocations = 1000;
    const uint64_t before = allocationCount();
    std::thread worker([] {
        for (uint64_t index = 0; index < kAllocations; ++index)
        {
            void* volatile block = std::malloc(64);
            std::free(block);
        }
    });
    worker.join();
    CHECK(allocationCount() - before >= kAllocations);
}

//======================================================================================================================
TEST_CASE("the host clock advances in nanoseconds", "[host]")
{
    const uint64_t start = hostTicks();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const uint64_t end = hostTicks();
    REQUIRE(end > start);
    const double nanoseconds = ticksToNanoseconds(end - start);
    CHECK(nanoseconds >= 9.0e6);
    // A loaded hosted runner may oversleep by far more than the request; only the lower bound is tight.
    CHECK(nanoseconds <= 500.0e6);
    CHECK(ticksToNanoseconds(0) == 0.0);
}

//======================================================================================================================
TEST_CASE("the thermal state is one of the four names", "[host]")
{
    const std::string_view state = thermalState();
    CHECK((state == "nominal" || state == "fair" || state == "serious" || state == "critical"));
}

//======================================================================================================================
TEST_CASE("the JSON writer escapes strings and writes null for a non-finite number", "[host]")
{
    JsonWriter json;
    json.beginObject();
    json.key("text").string("a\"b\\c\n\x01");
    json.key("values").beginArray();
    json.number(0.1).number(std::nan("")).number(std::numeric_limits<double>::infinity()).integer(-3).unsignedInteger(UINT64_MAX);
    json.boolean(true).null();
    json.endArray();
    json.key("empty").beginObject().endObject();
    json.endObject();
    CHECK(json.text() == R"({"text":"a\"b\\c\n\u0001","values":[0.1,null,null,-3,18446744073709551615,true,null],"empty":{}})");
}

//======================================================================================================================
TEST_CASE("the system device supports the Metal 4 family", "[native]")
{
    MTL::Device* device = MTL::CreateSystemDefaultDevice();
    REQUIRE(device != nullptr);
    CHECK(device->supportsFamily(MTL::GPUFamilyMetal4));
    device->release();
}

//======================================================================================================================
TEST_CASE("the environment command writes every contract field", "[native]")
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / ("rojorhi-bench-environment-" + std::to_string(getpid()) + ".json");
    const auto written = writeEnvironmentFile(path);
    REQUIRE(written.has_value());
    std::ifstream file(path);
    const std::string text{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    file.close();
    std::filesystem::remove(path);
    const std::set<std::string> expected{ "device", "families", "metal4", "timestampHz", "os", "thermal" };
    CHECK(topLevelKeys(text) == expected);
    CHECK(text.find("\"metal4\":true") != std::string::npos);
    CHECK(text.find("\"device\":null") == std::string::npos);
}
