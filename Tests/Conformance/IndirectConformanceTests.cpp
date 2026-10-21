//----------------------------------------------------------------------------------------------------------------------
/// @file IndirectConformanceTests.cpp
/// @brief Observes counted draws on a GPU through the draw oracle: records, counts, drawIds, splits, state, roots, passes and threads.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Conformance.h"
#include "Support/GpuKit.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The edge of the oracle targets, in pixels.
    constexpr uint32_t kEdge = 256;

    /// The most records whose oracle values an `RGBA16Float` target holds exactly: every drawId plus one is at most 2048.
    constexpr uint32_t kHalfExact = 2047;

    /// The viewport and scissor of the whole target.
    constexpr Viewport kFull{ .width = static_cast<float>(kEdge), .height = static_cast<float>(kEdge) };
    constexpr Rect2D kFullRect{ 0, 0, kEdge, kEdge };

    /// The cases' count variants: a null count, a count equal to `maxCount`, a quarter of it, zero, and one above it.
    enum class CountKind : uint8_t
    {
        Null,
        Full,
        Quarter,
        Zero,
        Above,
    };

    //==================================================================================================================
    /// The name of `kind`, for the case's capture.
    std::string_view nameOf(CountKind kind)
    {
        switch (kind)
        {
        case CountKind::Null: return "null";
        case CountKind::Full: return "full";
        case CountKind::Quarter: return "quarter";
        case CountKind::Zero: return "zero";
        case CountKind::Above: return "above";
        }
        return "";
    }

    //==================================================================================================================
    /// The count `kind` names for `maxCount` records.
    uint32_t countOf(CountKind kind, uint32_t maxCount)
    {
        switch (kind)
        {
        case CountKind::Null:
        case CountKind::Full: return maxCount;
        case CountKind::Quarter: return maxCount / 4;
        case CountKind::Zero: return 0;
        case CountKind::Above: return maxCount + 3;
        }
        return 0;
    }

    //==================================================================================================================
    /// The drawId of record `i` of `maxCount`, a power of two: a permutation of the records that no record keeps in place.
    uint32_t drawIdOf(uint32_t i, uint32_t maxCount)
    {
        return static_cast<uint32_t>((uint64_t{ 7919 } * i + 13) % maxCount);
    }

    //==================================================================================================================
    /// The half-precision bits of `value`, which is zero or a normal half-precision value held exactly.
    uint16_t halfOf(float value)
    {
        if (value == 0.0f)
        {
            return 0;
        }
        const auto bits = std::bit_cast<uint32_t>(value);
        const uint32_t sign = (bits >> 16) & 0x8000u;
        const auto exponent = static_cast<uint32_t>(static_cast<int32_t>((bits >> 23) & 0xFFu) - 127 + 15);
        return static_cast<uint16_t>(sign | (exponent << 10) | ((bits >> 13) & 0x3FFu));
    }

    /// An oracle target's image: four channels per texel as floats, of which an `R32Float` target keeps the first.
    struct Image
    {
        //==============================================================================================================
        /// Creates a zero image of a target of `format`.
        explicit Image(Format targetFormat) : format(targetFormat)
        {
        }

        //==============================================================================================================
        /// Sets the texel at cell `cell`, the oracle's pixel of a drawId plus 16,384 times the instance, to `value`.
        void set(uint32_t cell, const std::array<float, 4>& value)
        {
            texels[cell] = value;
        }

        //==============================================================================================================
        /// The bytes a target of `format` holds for this image.
        std::vector<std::byte> bytes() const
        {
            std::vector<std::byte> out;
            for (const std::array<float, 4>& texel : texels)
            {
                if (format == Format::R32Float)
                {
                    const auto* first = reinterpret_cast<const std::byte*>(&texel[0]);
                    out.insert(out.end(), first, first + sizeof(float));
                    continue;
                }
                for (const float channel : texel)
                {
                    const uint16_t half = halfOf(channel);
                    const auto* first = reinterpret_cast<const std::byte*>(&half);
                    out.insert(out.end(), first, first + sizeof(half));
                }
            }
            return out;
        }

        Format format;                                                                                         ///< The target's format.
        std::vector<std::array<float, 4>> texels = std::vector<std::array<float, 4>>(size_t{ kEdge } * kEdge); ///< In texel order.
    };

    //==================================================================================================================
    /// Describes where `actual`, the bytes of an oracle target of `format`, differs from `expected`, the first eight texels;
    /// empty when they are equal.
    std::string differences(const std::vector<std::byte>& actual, const Image& expected)
    {
        const std::vector<std::byte> want = expected.bytes();
        if (actual.size() != want.size())
        {
            return std::format("{} bytes read back, expected {}", actual.size(), want.size());
        }
        const size_t texelBytes = expected.format == Format::R32Float ? 4 : 8;
        std::string text;
        uint32_t found = 0;
        for (size_t texel = 0; texel < want.size() / texelBytes; ++texel)
        {
            const size_t at = texel * texelBytes;
            if (std::memcmp(&actual[at], &want[at], texelBytes) == 0)
            {
                continue;
            }
            if (++found <= 8)
            {
                uint64_t got = 0;
                uint64_t wanted = 0;
                std::memcpy(&got, &actual[at], texelBytes);
                std::memcpy(&wanted, &want[at], texelBytes);
                text += std::format("({}, {}): {:016x}, expected {:016x}\n", texel % kEdge, texel / kEdge, got, wanted);
            }
        }
        if (found > 0)
        {
            text += std::format("{} texels differ", found);
        }
        return text;
    }

    //==================================================================================================================
    /// Loads the conformance library `name` on `device`.
    ShaderLibrary loadLibrary(Device device, std::string_view name)
    {
        const std::vector<std::byte> blob = test::shaderLibraryFile(name);
        return *device.loadShaderLibrary(blob, name);
    }

    //==================================================================================================================
    /// The oracle's value for a triangle of `drawId`, instance 0 and first vertex `base`.
    std::array<float, 4> oracleValue(uint32_t drawId, uint32_t base = 0)
    {
        return { static_cast<float>(drawId + 1), 1.0f, static_cast<float>(base + 1), 1.0f };
    }

    //==================================================================================================================
    /// Returns the bytes of `values`.
    template<class T> std::vector<std::byte> bytesOf(std::span<T> values)
    {
        const std::span<const std::byte> bytes = std::as_bytes(values);
        return { bytes.begin(), bytes.end() };
    }

    /// The draw oracle on one kit: its library, pipelines over a target of a format, and the passes the cases record.
    struct Oracle
    {
        //==============================================================================================================
        /// Loads the draw oracle library on `kit`'s device.
        explicit Oracle(test::GpuKit& created) : kit(created), library(loadLibrary(created.device, "DrawOracle"))
        {
        }

        //==============================================================================================================
        /// The oracle's vertex pipeline over one target of `format` with `blend`, whose fragment entry is `fragment`.
        Pipeline pipeline(Format format, const BlendState& blend = {}, std::string_view fragment = "oracleFragmentMain") const
        {
            const std::array targets{ ColorTarget{ .format = format, .blend = blend } };
            Result<Pipeline> made = kit.device.createPipeline(
                GraphicsPipelineDesc{ .vertex = { library, "oracleVertexMain" }, .fragment = { library, fragment }, .colors = targets, .label = "test.oracle" }
            );
            if (!made)
            {
                FAIL(made.error().message);
            }
            return *made;
        }

        //==============================================================================================================
        /// Creates a `kEdge` square render target of `format`.
        Texture target(Format format) const
        {
            return *kit.device.createTexture(
                TextureDesc{
                    .type = TextureType::Tex2D, .format = format, .extent = { kEdge, kEdge, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" }
            );
        }

        //==============================================================================================================
        /// Begins a pass on `list` that clears `texture` to zero, then binds `bound`, the full viewport and the full scissor.
        static void begin(CommandList list, Texture texture, Pipeline bound)
        {
            const std::array colors{ ColorAttachment{ .texture = texture, .load = LoadOp::Clear } };
            list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = kFullRect, .label = "test.pass" });
            list.setPipeline(bound);
            list.setViewport(kFull);
            list.setScissor(kFullRect);
        }

        //==============================================================================================================
        /// Reads `texture`, an oracle target of `format`, back.
        std::vector<std::byte> read(Texture texture, Format format) const
        {
            return kit.readTexture(texture, TextureRegion{ .extent = { kEdge, kEdge, 1 } }, format);
        }

        //==============================================================================================================
        /// Creates an `IndirectArgs` buffer holding the `uint32` count `value`.
        Buffer count(uint32_t value) const
        {
            return kit.buffer(sizeof(value), Memory::Gpu, BufferUsage::IndirectArgs, std::as_bytes(std::span(&value, 1)));
        }

        test::GpuKit& kit;     ///< The kit.
        ShaderLibrary library; ///< The draw oracle library.
    };

    //==================================================================================================================
    /// `maxCount` records of three vertices and one instance from first vertex 0, record `i` with `drawIdOf(i, maxCount)`
    /// plus `firstDrawId` and first instance 1000 plus `i`.
    std::vector<DrawIndirectArgs> drawRecords(uint32_t maxCount, uint32_t firstDrawId = 0)
    {
        std::vector<DrawIndirectArgs> records;
        records.reserve(maxCount);
        for (uint32_t i = 0; i < maxCount; ++i)
        {
            records.push_back(
                DrawIndirectArgs{ .drawId = firstDrawId + drawIdOf(i, maxCount), .vertexCount = 3, .instanceCount = 1, .firstInstance = 1000 + i }
            );
        }
        return records;
    }

    //==================================================================================================================
    /// `image` with what `records` draw when the first `drawn` of them run.
    Image drawnImage(std::span<const DrawIndirectArgs> records, uint32_t drawn, Image image)
    {
        for (uint32_t i = 0; i < drawn; ++i)
        {
            image.set(records[i].drawId, oracleValue(records[i].drawId));
        }
        return image;
    }

    //==================================================================================================================
    /// The format of an oracle target for drawIds below `maxCount`: `RGBA16Float`, which also shows the instance and the
    /// first vertex, while it holds every value exactly, else `R32Float`, which holds the drawId alone.
    Format formatFor(uint32_t maxCount)
    {
        return maxCount <= kHalfExact ? Format::RGBA16Float : Format::R32Float;
    }
} // namespace

//======================================================================================================================
TEST_CASE("drawIndirect draws min(count, maxCount) records", "[conformance]")
{
    test::requireGpu();
    const uint32_t maxCount = GENERATE(1u, 16u, 256u, 4096u, 65536u);
    const auto kind = GENERATE(CountKind::Null, CountKind::Full, CountKind::Quarter, CountKind::Zero, CountKind::Above);
    CAPTURE(maxCount, nameOf(kind));
    test::GpuKit kit;
    const Oracle oracle(kit);
    const Format format = formatFor(maxCount);
    const Pipeline pipeline = oracle.pipeline(format);
    const Texture target = oracle.target(format);
    const std::vector<DrawIndirectArgs> records = drawRecords(maxCount);
    const Buffer args = kit.buffer(records.size() * sizeof(DrawIndirectArgs), Memory::Gpu, BufferUsage::IndirectArgs, bytesOf(std::span(records)));
    const uint32_t count = countOf(kind, maxCount);
    const Buffer counted = kind == CountKind::Null ? Buffer{} : oracle.count(count);
    CommandList list = kit.list(QueueType::Graphics, "test.indirect");
    Oracle::begin(list, target, pipeline);
    list.drawIndirect(kit.device.address(args), maxCount, kind == CountKind::Null ? Addr{} : kit.device.address(counted));
    list.endRenderPass();
    kit.run(std::array{ list });
    const std::string difference = differences(oracle.read(target, format), drawnImage(records, std::min(count, maxCount), Image(format)));
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("drawIndexedIndirect draws min(count, maxCount) records with firstIndex, baseVertex invisible", "[conformance]")
{
    test::requireGpu();
    const uint32_t maxCount = GENERATE(1u, 256u, 65536u);
    const IndexType type = GENERATE(IndexType::UInt16, IndexType::UInt32);
    const auto kind = GENERATE(CountKind::Null, CountKind::Quarter, CountKind::Above);
    CAPTURE(maxCount, static_cast<uint32_t>(type), nameOf(kind));
    test::GpuKit kit;
    const Oracle oracle(kit);
    const Format format = formatFor(maxCount);
    const Pipeline pipeline = oracle.pipeline(format);
    const Texture target = oracle.target(format);
    // Index k holds k + 6. Record i reads three of them from 3 (i mod 4) with baseVertex -6, 0 or 21, which vertexIndex
    // excludes: each triangle's vertices are the index values, consecutive from firstIndex + 6, a multiple of 3 and the
    // oracle's flat first vertex, which a lost firstIndex or an added baseVertex changes.
    constexpr std::array<int32_t, 3> kBaseVertices{ -6, 0, 21 };
    std::vector<std::byte> indexBytes;
    for (uint32_t k = 0; k < 12; ++k)
    {
        const uint32_t value = k + 6;
        if (type == IndexType::UInt16)
        {
            const auto narrow = static_cast<uint16_t>(value);
            indexBytes.insert(indexBytes.end(), reinterpret_cast<const std::byte*>(&narrow), reinterpret_cast<const std::byte*>(&narrow) + 2);
        }
        else
        {
            indexBytes.insert(indexBytes.end(), reinterpret_cast<const std::byte*>(&value), reinterpret_cast<const std::byte*>(&value) + 4);
        }
    }
    const Buffer indices = kit.buffer(indexBytes.size(), Memory::Gpu, BufferUsage::Index, indexBytes);
    std::vector<DrawIndexedIndirectArgs> records;
    Image expected(format);
    const uint32_t count = countOf(kind, maxCount);
    for (uint32_t i = 0; i < maxCount; ++i)
    {
        const uint32_t firstIndex = 3 * (i % 4);
        const int32_t baseVertex = kBaseVertices[i % 3];
        const uint32_t drawId = drawIdOf(i, maxCount);
        records.push_back(
            DrawIndexedIndirectArgs{
                .drawId = drawId, .indexCount = 3, .instanceCount = 1, .firstIndex = firstIndex, .baseVertex = baseVertex, .firstInstance = 1000 + i }
        );
        if (i < count)
        {
            expected.set(drawId, oracleValue(drawId, firstIndex + 6));
        }
    }
    const Buffer args = kit.buffer(records.size() * sizeof(DrawIndexedIndirectArgs), Memory::Gpu, BufferUsage::IndirectArgs, bytesOf(std::span(records)));
    const Buffer counted = kind == CountKind::Null ? Buffer{} : oracle.count(count);
    CommandList list = kit.list(QueueType::Graphics, "test.indirect");
    Oracle::begin(list, target, pipeline);
    list.drawIndexedIndirect(indices, type, kit.device.address(args), maxCount, kind == CountKind::Null ? Addr{} : kit.device.address(counted));
    list.endRenderPass();
    kit.run(std::array{ list });
    const std::string difference = differences(oracle.read(target, format), expected);
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("records and a count written by the GPU earlier in the list are read after the barrier", "[conformance]")
{
    test::requireGpu();
    const uint32_t maxCount = GENERATE(1u, 64u);
    CAPTURE(maxCount);
    test::GpuKit kit;
    const Oracle oracle(kit);
    const Format format = Format::RGBA16Float;
    const Pipeline pipeline = oracle.pipeline(format);
    const Texture target = oracle.target(format);
    const std::vector<DrawIndirectArgs> records = drawRecords(maxCount);
    const uint64_t bytes = records.size() * sizeof(DrawIndirectArgs);
    const Buffer staged = kit.buffer(bytes, Memory::Upload, BufferUsage::None, bytesOf(std::span(records)));
    // The records and the count start as zeroes, which draw nothing: only the GPU's writes make them draw.
    const std::vector<std::byte> zeroes(bytes, std::byte{ 0 });
    const Buffer args = kit.buffer(bytes, Memory::Gpu, BufferUsage::IndirectArgs, zeroes);
    // The fill kernel writes the count, so its buffer takes shader writes.
    const uint32_t zero = 0;
    const Buffer counted = kit.buffer(sizeof(zero), Memory::Gpu, BufferUsage::IndirectArgs | BufferUsage::ShaderWrite, std::as_bytes(std::span(&zero, 1)));
    const uint32_t count = maxCount == 1 ? 1 : 37;
    CommandList list = kit.list(QueueType::Graphics, "test.indirect");
    list.copyBuffer(staged, 0, args, 0, bytes);
    // A pattern whose bytes differ runs the fill kernel, a dispatch, rather than a blit fill.
    list.fillBuffer(counted, 0, sizeof(uint32_t), count);
    const std::array writes{
        BufferBarrier{
            .buffer = args, .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::IndirectArgs, .dstAccess = Access::IndirectArgs },
        BufferBarrier{
            .buffer = counted, .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::IndirectArgs, .dstAccess = Access::IndirectArgs },
    };
    list.barrier({}, writes);
    Oracle::begin(list, target, pipeline);
    list.drawIndirect(kit.device.address(args), maxCount, kit.device.address(counted));
    list.endRenderPass();
    kit.run(std::array{ list });
    const std::string difference = differences(oracle.read(target, format), drawnImage(records, std::min(count, maxCount), Image(format)));
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("a pass draws counted draws recorded before and after a direct draw and keeps its attachments and state", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracle oracle(kit);
    const Format format = Format::RGBA16Float;
    // Every draw writes its oracle value times the blend constants (0.5, 2, 0.25, 1), all of them exact in half precision; an
    // encoder that lost the constants writes zero, the clear value.
    const BlendState constant{
        .enable = true, .srcColor = BlendFactor::Constant, .dstColor = BlendFactor::Zero, .srcAlpha = BlendFactor::Constant, .dstAlpha = BlendFactor::Zero
    };
    const Pipeline pipeline = oracle.pipeline(format, constant);
    const Texture target = oracle.target(format);
    // Before the direct draw, drawIds 0 to 15; after it, drawIds 16 to 31, then drawId 8 alone through the writer kernel.
    const std::vector<DrawIndirectArgs> before = drawRecords(16);
    const std::vector<DrawIndirectArgs> after = drawRecords(16, 16);
    const std::vector<DrawIndirectArgs> lone = drawRecords(1, 8);
    const Buffer beforeArgs = kit.buffer(sizeof(DrawIndirectArgs) * 16, Memory::Gpu, BufferUsage::IndirectArgs, bytesOf(std::span(before)));
    const Buffer afterArgs = kit.buffer(sizeof(DrawIndirectArgs) * 16, Memory::Gpu, BufferUsage::IndirectArgs, bytesOf(std::span(after)));
    const Buffer loneArgs = kit.buffer(sizeof(DrawIndirectArgs), Memory::Gpu, BufferUsage::IndirectArgs, bytesOf(std::span(lone)));
    // The viewport and scissor of the counted draws after the direct one: shifted 32 texels right and 128 down, and clipped to
    // the 56 columns from 0 of the lower half, so drawIds 24 and above fall outside.
    constexpr uint32_t kShiftX = 32;
    constexpr uint32_t kShiftY = 128;
    constexpr Viewport kShifted{
        .x = static_cast<float>(kShiftX), .y = static_cast<float>(kShiftY), .width = static_cast<float>(kEdge), .height = static_cast<float>(kEdge)
    };
    constexpr Rect2D kClipped{ 0, kShiftY, 56, kEdge - kShiftY };
    CommandList list = kit.list(QueueType::Graphics, "test.indirect");
    Oracle::begin(list, target, pipeline);
    list.setBlendConstants(0.5f, 2.0f, 0.25f, 1.0f);
    list.drawIndirect(kit.device.address(beforeArgs), 16);
    // The direct draw, moved 64 rows down, opens the render encoder; the state set on it after stays in force, and the counted
    // draws after it split the pass twice. Each resumed encoder must load what the one before stored and hold the pipeline,
    // the blend constants, the viewport and the scissor.
    list.setViewport(Viewport{ .y = 64.0f, .width = static_cast<float>(kEdge), .height = static_cast<float>(kEdge) });
    list.draw(3);
    list.setViewport(kShifted);
    list.setScissor(kClipped);
    list.drawIndirect(kit.device.address(afterArgs), 16);
    list.draw(3, 1, 3);
    list.drawIndirect(kit.device.address(loneArgs), 1);
    list.endRenderPass();
    kit.run(std::array{ list });
    const auto blended = [](const std::array<float, 4>& value) { return std::array{ value[0] * 0.5f, value[1] * 2.0f, value[2] * 0.25f, value[3] }; };
    Image expected(format);
    for (const DrawIndirectArgs& record : before)
    {
        expected.set(record.drawId, blended(oracleValue(record.drawId)));
    }
    expected.set(64 * kEdge, blended(oracleValue(0)));
    for (const DrawIndirectArgs& record : after)
    {
        if (record.drawId + kShiftX < kClipped.width)
        {
            expected.set(kShiftY * kEdge + kShiftX + record.drawId, blended(oracleValue(record.drawId)));
        }
    }
    expected.set(kShiftY * kEdge + kShiftX, blended(oracleValue(0)));
    expected.set(kShiftY * kEdge + kShiftX + lone[0].drawId, blended(oracleValue(lone[0].drawId)));
    const std::string difference = differences(oracle.read(target, format), expected);
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("counted draws recorded before the render encoder opens use the pipeline bound before each", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracle oracle(kit);
    const Format format = Format::RGBA16Float;
    // The oracle's fragment entry writes the drawId; the solid one a constant color.
    const Pipeline ids = oracle.pipeline(format);
    const Pipeline solid = oracle.pipeline(format, {}, "solidFragmentMain");
    constexpr std::array kSolid{ 0.25f, 0.5f, 0.75f, 1.0f };
    const Texture target = oracle.target(format);
    // drawIds 0 to 3, 4 to 7, 8 alone through the writer kernel, and 9 to 12, with the pipelines alternating from `ids`.
    const std::array<std::vector<DrawIndirectArgs>, 4> records{ drawRecords(4), drawRecords(4, 4), drawRecords(1, 8), drawRecords(4, 9) };
    const std::array<Pipeline, 4> bound{ ids, solid, ids, solid };
    std::array<Buffer, 4> args{};
    for (size_t i = 0; i < records.size(); ++i)
    {
        args[i] = kit.buffer(sizeof(DrawIndirectArgs) * records[i].size(), Memory::Gpu, BufferUsage::IndirectArgs, bytesOf(std::span(records[i])));
    }
    CommandList list = kit.list(QueueType::Graphics, "test.indirect");
    Oracle::begin(list, target, ids);
    for (size_t i = 0; i < records.size(); ++i)
    {
        list.setPipeline(bound[i]);
        list.drawIndirect(kit.device.address(args[i]), static_cast<uint32_t>(records[i].size()));
    }
    list.endRenderPass();
    kit.run(std::array{ list });
    Image expected(format);
    for (size_t i = 0; i < records.size(); ++i)
    {
        for (const DrawIndirectArgs& record : records[i])
        {
            expected.set(record.drawId, i % 2 == 0 ? oracleValue(record.drawId) : kSolid);
        }
    }
    const std::string difference = differences(oracle.read(target, format), expected);
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("a record with no vertices, indices or instances draws nothing where the list drew one before", "[conformance]")
{
    test::requireGpu();
    const bool indexed = GENERATE(false, true);
    CAPTURE(indexed);
    // Two frame slots, so the third frame acquires the list of the first, whose indirect command buffers its counted draw
    // already filled with every record.
    constexpr uint32_t kSlots = 2;
    test::GpuKit kit(kSlots);
    const Oracle oracle(kit);
    const Format format = Format::RGBA16Float;
    const Pipeline pipeline = oracle.pipeline(format);
    constexpr uint32_t kRecords = 16;
    const std::vector<DrawIndirectArgs> drawn = drawRecords(kRecords);
    // In the third frame, records 1 and 5 have no vertices and records 3 and 7 no instances (no indices and no instances
    // when indexed).
    std::vector<DrawIndirectArgs> emptied = drawn;
    for (uint32_t i = 1; i < 8; i += 2)
    {
        (i % 4 == 1 ? emptied[i].vertexCount : emptied[i].instanceCount) = 0;
    }
    const auto indexedOf = [](std::span<const DrawIndirectArgs> records) {
        std::vector<DrawIndexedIndirectArgs> out;
        for (const DrawIndirectArgs& record : records)
        {
            out.push_back(
                DrawIndexedIndirectArgs{
                    .drawId = record.drawId, .indexCount = record.vertexCount, .instanceCount = record.instanceCount, .firstInstance = record.firstInstance }
            );
        }
        return out;
    };
    const auto argsOf = [&](std::span<const DrawIndirectArgs> records) {
        if (!indexed)
        {
            return kit.buffer(records.size() * sizeof(DrawIndirectArgs), Memory::Gpu, BufferUsage::IndirectArgs, bytesOf(records));
        }
        const std::vector<DrawIndexedIndirectArgs> converted = indexedOf(records);
        return kit.buffer(converted.size() * sizeof(DrawIndexedIndirectArgs), Memory::Gpu, BufferUsage::IndirectArgs, bytesOf(std::span(converted)));
    };
    const std::array<uint32_t, 3> indexValues{ 0, 1, 2 };
    const Buffer indices = kit.buffer(sizeof(indexValues), Memory::Gpu, BufferUsage::Index, std::as_bytes(std::span(indexValues)));
    const auto record = [&](Texture target, Buffer args) {
        CommandList list = kit.list(QueueType::Graphics, "test.indirect");
        Oracle::begin(list, target, pipeline);
        if (indexed)
        {
            list.drawIndexedIndirect(indices, IndexType::UInt32, kit.device.address(args), kRecords);
        }
        else
        {
            list.drawIndirect(kit.device.address(args), kRecords);
        }
        list.endRenderPass();
        kit.run(std::array{ list });
    };
    const Buffer drawnArgs = argsOf(drawn);
    const Buffer emptiedArgs = argsOf(emptied);
    record(oracle.target(format), drawnArgs);
    for (uint32_t frame = 0; frame < kSlots; ++frame)
    {
        kit.device.endFrame();
        kit.device.beginFrame();
    }
    const Texture target = oracle.target(format);
    record(target, emptiedArgs);
    Image expected(format);
    for (const DrawIndirectArgs& args : emptied)
    {
        if (args.vertexCount != 0 && args.instanceCount != 0)
        {
            expected.set(args.drawId, oracleValue(args.drawId));
        }
    }
    const std::string difference = differences(oracle.read(target, format), expected);
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("each counted draw sees the root recorded before it", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const ShaderLibrary oracle = loadLibrary(kit.device, "DrawOracle");
    const ShaderLibrary fragment = loadLibrary(kit.device, "AbiFragment");
    const Format format = Format::RGBA16Float;
    const std::array targets{ ColorTarget{ .format = format } };
    const Pipeline pipeline = *kit.device.createPipeline(
        GraphicsPipelineDesc{
            .vertex = { oracle, "oracleVertexMain" }, .fragment = { fragment, "fragmentMain" }, .colors = targets, .label = "test.abiFragment" }
    );
    // Two one-texel views of distinct values, one per root.
    const std::array<std::array<float, 4>, 2> values{ std::array{ 0.25f, 0.5f, 0.75f, 1.0f }, std::array{ 0.5f, 0.25f, 0.125f, 1.0f } };
    std::array<ViewIndex, 2> views{};
    for (size_t i = 0; i < 2; ++i)
    {
        const std::array<uint16_t, 4> texel{ halfOf(values[i][0]), halfOf(values[i][1]), halfOf(values[i][2]), halfOf(values[i][3]) };
        const std::array data{ TextureData{ std::as_bytes(std::span(texel)), sizeof(texel), 0 } };
        const Texture sampled =
            *kit.device.createTexture(TextureDesc{ .format = format, .extent = { 1, 1, 1 }, .usage = TextureUsage::Sampled, .label = "test.texel" }, data);
        views[i] = *kit.device.createView(sampled, {});
    }
    const SamplerIndex sampler =
        *kit.device.createSampler(SamplerDesc{ .filter = SamplerFilter::Nearest, .mipFilter = SamplerFilter::Nearest, .address = SamplerAddress::ClampToEdge });
    const Texture target =
        *kit.device.createTexture(TextureDesc{ .format = format, .extent = { kEdge, kEdge, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" });
    // The first and third counted draws read the first root, the second the other; the third is a lone record.
    const std::vector<DrawIndirectArgs> first = drawRecords(4);
    const std::vector<DrawIndirectArgs> second = drawRecords(4, 4);
    const std::vector<DrawIndirectArgs> third = drawRecords(1, 8);
    const Buffer firstArgs = kit.buffer(sizeof(DrawIndirectArgs) * 4, Memory::Gpu, BufferUsage::IndirectArgs, bytesOf(std::span(first)));
    const Buffer secondArgs = kit.buffer(sizeof(DrawIndirectArgs) * 4, Memory::Gpu, BufferUsage::IndirectArgs, bytesOf(std::span(second)));
    const Buffer thirdArgs = kit.buffer(sizeof(DrawIndirectArgs), Memory::Gpu, BufferUsage::IndirectArgs, bytesOf(std::span(third)));
    /// AbiFragment's root.
    struct FragmentRoot
    {
        uint32_t view;    ///< The sampled view.
        uint32_t sampler; ///< The sampler.
        uint32_t pad0;    ///< Padding.
        uint32_t pad1;    ///< Padding.
    };
    CommandList list = kit.list(QueueType::Graphics, "test.indirect");
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = kFullRect, .label = "test.pass" });
    list.setPipeline(pipeline);
    list.setViewport(kFull);
    list.setScissor(kFullRect);
    const Addr firstRoot = list.pushRoot(FragmentRoot{ static_cast<uint32_t>(views[0]), static_cast<uint32_t>(sampler), 0, 0 });
    list.drawIndirect(kit.device.address(firstArgs), 4);
    list.pushRoot(FragmentRoot{ static_cast<uint32_t>(views[1]), static_cast<uint32_t>(sampler), 0, 0 });
    list.drawIndirect(kit.device.address(secondArgs), 4);
    list.setRoot(firstRoot);
    list.drawIndirect(kit.device.address(thirdArgs), 1);
    list.endRenderPass();
    kit.run(std::array{ list });
    Image expected(format);
    const auto add = [&expected](std::span<const DrawIndirectArgs> records, const std::array<float, 4>& value) {
        for (const DrawIndirectArgs& record : records)
        {
            expected.set(record.drawId, { value[0] + static_cast<float>(record.drawId), value[1], value[2], value[3] });
        }
    };
    add(first, values[0]);
    add(second, values[1]);
    add(third, values[0]);
    const std::string difference = differences(kit.readTexture(target, TextureRegion{ .extent = { kEdge, kEdge, 1 } }, format), expected);
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("counted draws in two passes of one list and in lists recorded on two threads", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    const Oracle oracle(kit);
    const Format format = Format::RGBA16Float;
    const Pipeline pipeline = oracle.pipeline(format);
    // Each pass draws its own drawIds into its own target, with a counted draw of each path.
    constexpr uint32_t kPasses = 3;
    std::array<Texture, kPasses> targets{};
    std::array<std::vector<DrawIndirectArgs>, kPasses> records;
    std::array<Buffer, kPasses> args{};
    std::array<Buffer, kPasses> lone{};
    std::array<std::vector<DrawIndirectArgs>, kPasses> loneRecords;
    for (uint32_t pass = 0; pass < kPasses; ++pass)
    {
        targets[pass] = oracle.target(format);
        records[pass] = drawRecords(32, 100 * pass);
        loneRecords[pass] = drawRecords(1, 100 * pass + 50);
        args[pass] = kit.buffer(sizeof(DrawIndirectArgs) * 32, Memory::Gpu, BufferUsage::IndirectArgs, bytesOf(std::span(records[pass])));
        lone[pass] = kit.buffer(sizeof(DrawIndirectArgs), Memory::Gpu, BufferUsage::IndirectArgs, bytesOf(std::span(loneRecords[pass])));
    }
    const auto record = [&](CommandList list, uint32_t pass) {
        Oracle::begin(list, targets[pass], pipeline);
        list.drawIndirect(kit.device.address(args[pass]), 32);
        list.drawIndirect(kit.device.address(lone[pass]), 1);
        list.endRenderPass();
    };
    CommandList twoPasses = kit.list(QueueType::Graphics, "test.twoPasses");
    CommandList threaded = kit.list(QueueType::Graphics, "test.threaded");
    std::thread worker([&] { record(threaded, 2); });
    record(twoPasses, 0);
    record(twoPasses, 1);
    worker.join();
    kit.run(std::array{ twoPasses, threaded });
    for (uint32_t pass = 0; pass < kPasses; ++pass)
    {
        CAPTURE(pass);
        Image expected = drawnImage(records[pass], 32, Image(format));
        expected = drawnImage(loneRecords[pass], 1, expected);
        const std::string difference = differences(oracle.read(targets[pass], format), expected);
        INFO(difference);
        CHECK(difference.empty());
    }
}
