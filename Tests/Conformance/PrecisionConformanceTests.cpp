//----------------------------------------------------------------------------------------------------------------------
/// @file PrecisionConformanceTests.cpp
/// @brief Checks that a dynamically loaded fp32 multiply and add remain separate under precise shader compilation.
//----------------------------------------------------------------------------------------------------------------------
#include "AbiExamples.h"
#include "Support/Backend.h"
#include "Support/Conformance.h"
#include "Support/GpuKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>

using namespace rojoRHI;

namespace {
    struct PreciseProgram
    {
        Device device;
        ShaderLibrary library{};
        Pipeline pipeline{};

        ~PreciseProgram();
    };

    //==================================================================================================================
    PreciseProgram::~PreciseProgram()
    {
        if (pipeline.bits != 0)
        {
            device.destroy(pipeline);
        }
        if (library.bits != 0)
        {
            device.destroy(library);
        }
    }
} // namespace

//======================================================================================================================
TEST_CASE("AbiPrecise preserves separate dynamic fp32 multiply and add", "[conformance][precision]")
{
    test::requireGpu();
    test::GpuKit kit;
    PreciseProgram program{ kit.device };
    const auto library = kit.device.loadShaderLibrary(test::shaderLibraryFile("AbiPrecise"), "test.precise.library");
    REQUIRE(library);
    program.library = *library;
    const auto pipeline = kit.device.createPipeline(ComputePipelineDesc{ .compute = { program.library, "preciseMain" }, .label = "test.precise.pipeline" });
    REQUIRE(pipeline);
    program.pipeline = *pipeline;

    // (1 + 2^-23) * (1 - 2^-23) rounds to 1 before subtracting 1; contraction would produce -2^-46.
    const std::array<uint32_t, 4> inputBits{ 0x3F800001u, 0x3F7FFFFEu, 0xBF800000u, 0u };
    const std::array<uint32_t, 1> untouched{ 0xDEADBEEFu };
    const Buffer input = kit.buffer(sizeof(inputBits), Memory::Gpu, BufferUsage::None, std::as_bytes(std::span{ inputBits }));
    const Buffer output = kit.buffer(sizeof(untouched), Memory::Gpu, BufferUsage::ShaderWrite, std::as_bytes(std::span{ untouched }));
    CommandList list = kit.list(QueueType::Graphics, "test.precise");
    list.setPipeline(program.pipeline);
    const Addr root = list.pushRoot(test::PreciseRoot{ .input = kit.device.address(input), .output = kit.device.address(output) });
#if defined(ROJORHI_TEST_BACKEND_D3D12)
    CHECK(root.hi % 256 == 0);
#else
    CHECK(root.lo % 256 == 0);
#endif
    list.dispatch(1, 1, 1);
    kit.run(std::array{ list });
    const auto bytes = kit.readBuffer(output);
    REQUIRE(bytes.size() == sizeof(uint32_t));
    uint32_t resultBits = 0xDEADBEEFu;
    std::memcpy(&resultBits, bytes.data(), sizeof(resultBits));
    CHECK(resultBits == 0u);
}
