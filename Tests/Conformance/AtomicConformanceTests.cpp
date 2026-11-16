//----------------------------------------------------------------------------------------------------------------------
/// @file AtomicConformanceTests.cpp
/// @brief Checks contended uint atomic additions, old values, wraparound, offsets and divergent target buffers on a GPU.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "AbiExamples.h"
#include "Support/Conformance.h"
#include "Support/GpuKit.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

using namespace rojoRHI;

namespace {
    /// All words other than the one atomic target retain this value.
    constexpr uint32_t kGuard = 0xDEADBEEFu;
    constexpr uint32_t kWords = 16;
    constexpr uint64_t kBaseOffset = 16;
    constexpr uint32_t kIndex = 3;
    constexpr uint32_t kTargetWord = kBaseOffset / sizeof(uint32_t) + kIndex;
    constexpr uint32_t kGroupSize = 64;

#ifndef NDEBUG
    constexpr bool kValidation = true;
#else
    constexpr bool kValidation = false;
#endif

    /// Retires the atomic case's own pipeline before its library, including assertion-unwind paths.
    struct AtomicObjects
    {
        Device device;
        ShaderLibrary library;
        Pipeline pipeline;

        ~AtomicObjects();
    };

    //==================================================================================================================
    AtomicObjects::~AtomicObjects()
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

    //==================================================================================================================
    /// Reads every uint word of a buffer through the kit's copy/readback path.
    std::vector<uint32_t> readWords(test::GpuKit& kit, Buffer buffer)
    {
        const std::vector<std::byte> bytes = kit.readBuffer(buffer);
        REQUIRE(bytes.size() % sizeof(uint32_t) == 0);
        std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
        std::memcpy(words.data(), bytes.data(), bytes.size());
        return words;
    }

    //==================================================================================================================
    /// Copies an initialized word array to a new shader-write buffer in the tested list, before its initialization barrier.
    Buffer targetBuffer(test::GpuKit& kit, CommandList list, uint32_t initial)
    {
        std::array<uint32_t, kWords> words;
        words.fill(kGuard);
        words[kTargetWord] = initial;
        const Buffer upload = kit.buffer(sizeof(words), Memory::Upload, BufferUsage::None, std::as_bytes(std::span(words)));
        const Buffer target = kit.buffer(sizeof(words), Memory::Gpu, BufferUsage::ShaderWrite);
        list.copyBuffer(upload, 0, target, 0, sizeof(words));
        return target;
    }

    //==================================================================================================================
    /// Dispatches contended additions with declared accesses and explicit initialization/readback barriers. Checks all guard
    /// words and returns one old word per invocation, in invocation order, for the permutation oracle in each case.
    std::vector<uint32_t> additions(test::GpuKit& kit, uint32_t count, uint32_t initial, uint32_t increment, uint32_t final, bool divergent)
    {
        const std::vector<std::byte> blob = test::shaderLibraryFile("AbiAtomic");
        AtomicObjects objects{ .device = kit.device };
        objects.library = *kit.device.loadShaderLibrary(blob, "test.atomic");
        objects.pipeline = *kit.device.createPipeline(
            ComputePipelineDesc{ .compute = { objects.library, divergent ? "divergentMain" : "fanInMain" }, .label = "test.atomic" }
        );
        CommandList list = kit.list(QueueType::Graphics, "test.atomic");
        std::vector<Buffer> targets{ targetBuffer(kit, list, initial) };
        if (divergent)
        {
            targets.push_back(targetBuffer(kit, list, initial));
        }
        const uint64_t outputBytes = uint64_t{ count + kWords } * sizeof(uint32_t);
        const Buffer output = kit.buffer(outputBytes, Memory::Gpu, BufferUsage::ShaderWrite);
        list.fillBuffer(output, 0, outputBytes, kGuard);
        Addr base = kit.device.address(targets[0], kBaseOffset);
        std::vector<ResourceAccess> accesses;
        std::vector<BufferBarrier> initialized;
        for (Buffer target : targets)
        {
            accesses.push_back(
                ResourceAccess{ .buffer = target,
                                .offset = kTargetWord * sizeof(uint32_t),
                                .size = sizeof(uint32_t),
                                .stage = Stage::ComputeShader,
                                .access = Access::ShaderWrite }
            );
            initialized.push_back(
                BufferBarrier{ .buffer = target,
                               .size = kWords * sizeof(uint32_t),
                               .srcStage = Stage::Copy,
                               .srcAccess = Access::CopyWrite,
                               .dstStage = Stage::ComputeShader,
                               .dstAccess = Access::ShaderWrite }
            );
        }
        if (divergent)
        {
            const std::array addresses{ base, kit.device.address(targets[1], kBaseOffset) };
            const Buffer upload = kit.buffer(sizeof(addresses), Memory::Upload, BufferUsage::None, std::as_bytes(std::span(addresses)));
            const Buffer table = kit.buffer(sizeof(addresses), Memory::Gpu);
            list.copyBuffer(upload, 0, table, 0, sizeof(addresses));
            base = kit.device.address(table);
            accesses.push_back(ResourceAccess{ .buffer = table, .size = sizeof(addresses), .stage = Stage::ComputeShader, .access = Access::ShaderRead });
            initialized.push_back(
                BufferBarrier{ .buffer = table,
                               .size = sizeof(addresses),
                               .srcStage = Stage::Copy,
                               .srcAccess = Access::CopyWrite,
                               .dstStage = Stage::ComputeShader,
                               .dstAccess = Access::ShaderRead }
            );
        }
        accesses.push_back(
            ResourceAccess{ .buffer = output, .size = uint64_t{ count } * sizeof(uint32_t), .stage = Stage::ComputeShader, .access = Access::ShaderWrite }
        );
        initialized.push_back(
            BufferBarrier{ .buffer = output,
                           .size = outputBytes,
                           .srcStage = Stage::Copy,
                           .srcAccess = Access::CopyWrite,
                           .dstStage = Stage::ComputeShader,
                           .dstAccess = Access::ShaderWrite }
        );
        list.barrier({}, initialized);
        list.setPipeline(objects.pipeline);
        const test::AtomicRoot root{ .target = base, .output = kit.device.address(output), .count = count, .index = kIndex, .value = increment };
        // pushRoot binds these actual bytes from the frame arena at 256-byte placement, checked by Debug validation.
        list.pushRoot(root);
        list.declareAccess(accesses);
        REQUIRE(count % kGroupSize == 0);
        list.dispatch(count / kGroupSize, 1, 1);
        // Include the initialization copies as well as the atomic writes: the guards were never touched by the dispatch.
        list.barrier(
            std::array{ GlobalBarrier{ .srcStage = Stage::Copy | Stage::ComputeShader,
                                       .srcAccess = Access::CopyWrite | Access::ShaderWrite,
                                       .dstStage = Stage::Copy,
                                       .dstAccess = Access::CopyRead } }
        );
        kit.run(std::array{ list });
        for (size_t target = 0; target < targets.size(); ++target)
        {
            const std::vector<uint32_t> words = readWords(kit, targets[target]);
            REQUIRE(words.size() == kWords);
            for (uint32_t word = 0; word < kWords; ++word)
            {
                CAPTURE(target, word);
                CHECK(words[word] == (word == kTargetWord ? final : kGuard));
            }
        }
        std::vector<uint32_t> old = readWords(kit, output);
        REQUIRE(old.size() == count + kWords);
        for (uint32_t word = count; word < count + kWords; ++word)
        {
            CAPTURE(word);
            CHECK(old[word] == kGuard);
        }
        old.resize(count);
        return old;
    }
} // namespace

//======================================================================================================================
TEST_CASE("atomicAdd preserves every contended old value at an offset", "[conformance][atomic]")
{
    test::requireGpu();
    const uint32_t increment = GENERATE(1u, 3u);
    CAPTURE(increment);
    test::GpuKit kit(2, false, kValidation);
    const uint32_t final = increment == 1 ? 4113u : 12305u;
    std::vector<uint32_t> old = additions(kit, 4096, 17, increment, final, false);
    std::ranges::sort(old);
    for (uint32_t k = 0; k < 4096; ++k)
    {
        CAPTURE(k);
        CHECK(old[k] == 17 + increment * k);
    }
}

//======================================================================================================================
TEST_CASE("atomicAdd wraps uint additions and returned values", "[conformance][atomic]")
{
    test::requireGpu();
    test::GpuKit kit(2, false, kValidation);
    constexpr uint32_t kInitial = 0xFFFFFF00u;
    std::vector<uint32_t> old = additions(kit, 1024, kInitial, 1, 768, false);
    for (uint32_t& value : old)
    {
        value -= kInitial;
    }
    std::ranges::sort(old);
    for (uint32_t k = 0; k < 1024; ++k)
    {
        CAPTURE(k);
        CHECK(old[k] == k);
    }
}

//======================================================================================================================
TEST_CASE("atomicAdd selects divergent buffers before contended additions", "[conformance][atomic]")
{
    test::requireGpu();
    test::GpuKit kit(2, false, kValidation);
    const std::vector<uint32_t> old = additions(kit, 4096, 17, 1, 2065, true);
    for (uint32_t parity = 0; parity < 2; ++parity)
    {
        std::vector<uint32_t> selected;
        for (uint32_t thread = parity; thread < 4096; thread += 2)
        {
            selected.push_back(old[thread]);
        }
        std::ranges::sort(selected);
        REQUIRE(selected.size() == 2048);
        for (uint32_t k = 0; k < 2048; ++k)
        {
            CAPTURE(parity, k);
            CHECK(selected[k] == 17 + k);
        }
    }
}
