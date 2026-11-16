//----------------------------------------------------------------------------------------------------------------------
/// @file PrecisionConformanceTests.cpp
/// @brief Checks on a GPU that a file built under slang.precise_files rounds a multiply before the add that follows it.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "AbiExamples.h"
#include "Support/Backend.h"
#include "Support/Conformance.h"
#include "Support/GpuKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The bits of -2^-46, which the case's expression gives when its multiply and add are fused.
    constexpr uint32_t kFused = 0xA8800000u;
} // namespace

//======================================================================================================================
TEST_CASE("a precise file rounds a multiply before the add", "[conformance][precision]")
{
    test::requireGpu();
    test::GpuKit kit;
    const std::vector<std::byte> blob = test::shaderLibraryFile("AbiPrecise");
    const Result<ShaderLibrary> library = kit.device.loadShaderLibrary(blob, "test.precise");
    REQUIRE(library);
    const Result<Pipeline> pipeline = kit.device.createPipeline(ComputePipelineDesc{ .compute = { *library, "preciseMain" }, .label = "test.precise" });
    REQUIRE(pipeline);

    // (1 + 2^-23) * (1 - 2^-23) is 1 - 2^-46, which rounds to 1, and adding -1 gives 0; a fused multiply-add keeps the
    // product unrounded and gives -2^-46.
    const std::array<uint32_t, 4> inputBits{ 0x3F800001u, 0x3F7FFFFEu, 0xBF800000u, 0u };
    const std::array<uint32_t, 1> untouched{ 0xDEADBEEFu };
    const Buffer input = kit.buffer(sizeof(inputBits), Memory::Gpu, BufferUsage::None, std::as_bytes(std::span{ inputBits }));
    const Buffer output = kit.buffer(sizeof(untouched), Memory::Gpu, BufferUsage::ShaderWrite, std::as_bytes(std::span{ untouched }));
    CommandList list = kit.list(QueueType::Graphics, "test.precise");
    list.setPipeline(*pipeline);
    list.pushRoot(test::PreciseRoot{ .input = kit.device.address(input), .output = kit.device.address(output) });
    list.dispatch(1, 1, 1);
    kit.run(std::array{ list });

    const std::vector<std::byte> bytes = kit.readBuffer(output);
    REQUIRE(bytes.size() == sizeof(uint32_t));
    uint32_t resultBits = 0;
    std::memcpy(&resultBits, bytes.data(), sizeof(resultBits));
    const std::string_view reason = test::fusesPreciseArithmetic();
    if (!reason.empty())
    {
        // Where the device is known to fuse, only the exact and the fused results are expected; any other value fails.
        CAPTURE(resultBits);
        REQUIRE((resultBits == 0u || resultBits == kFused));
        if (resultBits == kFused)
        {
            SKIP("the result is the fused -2^-46, and " << reason);
        }
    }
    CHECK(resultBits == 0u);
}
