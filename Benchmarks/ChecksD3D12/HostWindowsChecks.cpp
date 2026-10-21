//----------------------------------------------------------------------------------------------------------------------
/// @file HostWindowsChecks.cpp
/// @brief Checks the Windows host services: the allocation counter, the clock, the thermal state and N's message callback.
//----------------------------------------------------------------------------------------------------------------------
#include "Host/AllocationCounter.h"
#include "Host/HostClock.h"
#include "Host/ThermalState.h"
#include "NativeD3D12/NativeContext.h"

#include <catch2/catch_test_macros.hpp>

#include <process.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <new>
#include <string>
#include <string_view>
#include <thread>

using namespace rojoRHI::bench;

namespace {
    // The allocations the worker thread of the cross-thread case makes.
    constexpr uint64_t kThreadAllocations = 1000;

    // A type whose alignment exceeds the default new alignment, so new of it calls the aligned forms.
    struct alignas(64) Aligned
    {
        std::byte bytes[64];
    };

    //==================================================================================================================
    // Every byte written to `file`, read from its start.
    std::string contents(std::FILE* file)
    {
        std::fflush(file);
        std::rewind(file);
        std::string text;
        char buffer[256];
        size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        {
            text.append(buffer, read);
        }
        return text;
    }
} // namespace

//======================================================================================================================
TEST_CASE("the Windows allocation counter counts each operator new form once", "[host]")
{
    REQUIRE(installAllocationCounter().has_value());
    // Each result is stored through a volatile pointer, so the compiler cannot elide the allocation; nothing between a
    // count and the allocation it brackets allocates.
    uint64_t before = allocationCount();
    int* volatile scalar = new int(1);
    const uint64_t afterNew = allocationCount() - before;

    before = allocationCount();
    int* volatile numbers = new int[4];
    const uint64_t afterArray = allocationCount() - before;

    before = allocationCount();
    Aligned* volatile aligned = new Aligned();
    const uint64_t afterAligned = allocationCount() - before;

    before = allocationCount();
    Aligned* volatile alignedArray = new Aligned[2];
    const uint64_t afterAlignedArray = allocationCount() - before;

    before = allocationCount();
    int* volatile nothrowScalar = new (std::nothrow) int(1);
    const uint64_t afterNothrow = allocationCount() - before;

    before = allocationCount();
    int* volatile nothrowArray = new (std::nothrow) int[4];
    const uint64_t afterNothrowArray = allocationCount() - before;

    before = allocationCount();
    Aligned* volatile nothrowAligned = new (std::nothrow) Aligned();
    const uint64_t afterNothrowAligned = allocationCount() - before;

    before = allocationCount();
    Aligned* volatile nothrowAlignedArray = new (std::nothrow) Aligned[2];
    const uint64_t afterNothrowAlignedArray = allocationCount() - before;

    before = allocationCount();
    delete scalar;
    delete[] numbers;
    delete aligned;
    delete[] alignedArray;
    delete nothrowScalar;
    delete[] nothrowArray;
    delete nothrowAligned;
    delete[] nothrowAlignedArray;
    const uint64_t afterDeletes = allocationCount() - before;

    CHECK(afterNew == 1);
    CHECK(afterArray == 1);
    CHECK(afterAligned == 1);
    CHECK(afterAlignedArray == 1);
    CHECK(afterNothrow == 1);
    CHECK(afterNothrowArray == 1);
    CHECK(afterNothrowAligned == 1);
    CHECK(afterNothrowAlignedArray == 1);
    CHECK(afterDeletes == 0);
}

//======================================================================================================================
TEST_CASE("the Windows allocation counter sees allocations of other threads", "[host]")
{
    REQUIRE(installAllocationCounter().has_value());
    const uint64_t before = allocationCount();
    std::thread worker([] {
        for (uint64_t index = 0; index < kThreadAllocations; ++index)
        {
            int* volatile block = new int(static_cast<int>(index));
            delete block;
        }
    });
    worker.join();
    CHECK(allocationCount() - before >= kThreadAllocations);
}

//======================================================================================================================
TEST_CASE("the host clock advances in nanoseconds", "[host]")
{
    const uint64_t start = hostTicks();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const uint64_t end = hostTicks();
    REQUIRE(end > start);
    const double nanoseconds = ticksToNanoseconds(end - start);
    CHECK(nanoseconds >= 1.0e6);
    // A loaded hosted runner may oversleep by far more than the request; only the lower bound is tight.
    CHECK(nanoseconds <= 1.0e9);
    CHECK(ticksToNanoseconds(0) == 0.0);
}

//======================================================================================================================
TEST_CASE("the thermal state on Windows is unknown", "[host]")
{
    CHECK(thermalState() == "unknown");
}

//======================================================================================================================
TEST_CASE("N's message callback writes one standard-error line per warning or error", "[host]")
{
    // N registers the callback with standard error as its sink; a file stands in for it here.
    const std::filesystem::path path = std::filesystem::temp_directory_path() / ("rojorhi-bench-messages-" + std::to_string(_getpid()) + ".txt");
    std::FILE* sink = nullptr;
    REQUIRE(_wfopen_s(&sink, path.c_str(), L"w+b") == 0);
    REQUIRE(sink != nullptr);
    const auto message = [&](D3D12_MESSAGE_SEVERITY severity, int id, const char* text) {
        nativeMessage(D3D12_MESSAGE_CATEGORY_EXECUTION, severity, static_cast<D3D12_MESSAGE_ID>(id), text, sink);
    };
    message(D3D12_MESSAGE_SEVERITY_WARNING, 1234, "a synthetic warning");
    message(D3D12_MESSAGE_SEVERITY_INFO, 1235, "an information message");
    message(D3D12_MESSAGE_SEVERITY_MESSAGE, 1236, "a plain message");
    message(D3D12_MESSAGE_SEVERITY_ERROR, 1237, "a synthetic error");
    message(D3D12_MESSAGE_SEVERITY_CORRUPTION, 1238, "a synthetic corruption");
    message(D3D12_MESSAGE_SEVERITY_WARNING, D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE, "a denied warning");
    const std::string text = contents(sink);
    std::fclose(sink);
    std::filesystem::remove(path);

    CHECK(text == "D3D12 WARNING 1234: a synthetic warning\nD3D12 ERROR 1237: a synthetic error\nD3D12 CORRUPTION 1238: a synthetic corruption\n");
    CHECK(nativeMessageDenied(D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE));
    CHECK(nativeMessageDenied(D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE));
    CHECK(nativeMessageDenied(D3D12_MESSAGE_ID_NON_OPTIMAL_BARRIER_ONLY_EXECUTE_COMMAND_LISTS));
    CHECK(!nativeMessageDenied(static_cast<D3D12_MESSAGE_ID>(1234)));
}
