//----------------------------------------------------------------------------------------------------------------------
/// @file MeshConformanceTests.cpp
/// @brief Observes indirect mesh draws on a GPU through the mesh oracle: lone records, counts, groups, drawIds, barriers, roots and zero records.
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
#include <vector>

using namespace rojoRHI;

namespace {
    /// The edge of the oracle targets, in pixels.
    constexpr uint32_t kEdge = 256;

    /// The cells one step of a task group's x advances in the oracle, and those one step of `y + 2 z` advances: group
    /// `(x, y, z)` of a draw lands on cell `drawId + kGroupCells * x + kPlaneCells * (y + 2 z)`.
    constexpr uint32_t kGroupCells = 16384;
    constexpr uint32_t kPlaneCells = 4096;

    /// The task groups each record of these cases launches.
    constexpr uint32_t kGroups = 2;

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
    /// The drawId of record `i` of `maxCount`, a power of two: a permutation of the records that no record keeps in place
    /// above one record.
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
        /// Sets the cell task group `(group, y, z)` of a draw with `drawId` covers to what the oracle's fragment entry writes.
        void draw(uint32_t drawId, uint32_t group, uint32_t y = 0, uint32_t z = 0)
        {
            set(drawId + kGroupCells * group + kPlaneCells * (y + 2 * z), { static_cast<float>(drawId + 1), static_cast<float>(group + 1), 0.0f, 1.0f });
        }

        //==============================================================================================================
        /// Sets the texel at cell `cell` to `value`.
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
    /// Describes where `actual`, the bytes of an oracle target, differs from `expected`, the first eight texels; empty when
    /// they are equal.
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
    /// Returns the bytes of `values`.
    template<class T> std::vector<std::byte> bytesOf(std::span<T> values)
    {
        const std::span<const std::byte> bytes = std::as_bytes(values);
        return { bytes.begin(), bytes.end() };
    }

    /// The root of `MeshRootOracle`, which each of its stages reads.
    struct MeshRoot
    {
        uint32_t view;         ///< A 16 by 1 `R32Float` view.
        uint32_t sampler;      ///< A nearest sampler.
        uint32_t taskBias;     ///< Added by the task entry to the sampled value.
        uint32_t meshShift;    ///< Added by the mesh entry to the cell.
        uint32_t fragmentBias; ///< Added by the fragment entry to the group.
        uint32_t pad[3];       ///< Padding.
    };

    //==================================================================================================================
    /// Skips the running case when the device draws no indirect mesh tasks: the core refuses the call there.
    void requireMeshTasksIndirect(Device device)
    {
        if (!device.capabilities().meshTasksIndirect)
        {
            SKIP("the device does not report meshTasksIndirect, so drawMeshTasksIndirect cannot be recorded");
        }
    }

    /// The mesh oracle on one kit: its library, pipelines over a target of a format, and the passes the cases record.
    struct Oracle
    {
        //==============================================================================================================
        /// Loads the mesh oracle library on `kit`'s device.
        explicit Oracle(test::GpuKit& created) : kit(created)
        {
            const std::vector<std::byte> blob = test::shaderLibraryFile("MeshOracle");
            library = *kit.device.loadShaderLibrary(blob, "MeshOracle");
        }

        //==============================================================================================================
        /// The oracle's task, mesh and fragment pipeline over one target of `format`; without the task entry, and with the
        /// mesh entry that reads the drawId itself, when `taskless` is set.
        Pipeline pipeline(Format format, bool taskless = false) const
        {
            const std::array targets{ ColorTarget{ .format = format } };
            Result<Pipeline> made = kit.device.createPipeline(
                GraphicsPipelineDesc{ .task = taskless ? ShaderEntry{} : ShaderEntry{ library, "oracleTaskMain" },
                                      .mesh = { library, taskless ? "oracleMeshOnlyMain" : "oracleMeshMain" },
                                      .fragment = { library, "meshFragmentMain" },
                                      .colors = targets,
                                      .label = "test.meshOracle" }
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
        ShaderLibrary library; ///< The mesh oracle library.
    };

    //==================================================================================================================
    /// `maxCount` records of `kGroups` task groups, record `i` with `drawIdOf(i, maxCount)` plus `firstDrawId`. When `varied`
    /// is set, record `i` also has 1 + bit 0 of `i` groups in y and 1 + bit 1 of `i` in z.
    std::vector<DrawMeshTasksIndirectArgs> meshRecords(uint32_t maxCount, uint32_t firstDrawId = 0, bool varied = false)
    {
        std::vector<DrawMeshTasksIndirectArgs> records;
        records.reserve(maxCount);
        for (uint32_t i = 0; i < maxCount; ++i)
        {
            const uint32_t y = varied ? 1 + (i & 1) : 1;
            const uint32_t z = varied ? 1 + ((i >> 1) & 1) : 1;
            records.push_back(DrawMeshTasksIndirectArgs{ .drawId = firstDrawId + drawIdOf(i, maxCount), .x = kGroups, .y = y, .z = z });
        }
        return records;
    }

    //==================================================================================================================
    /// `image` with what `records` draw when the first `drawn` of them run.
    Image drawnImage(std::span<const DrawMeshTasksIndirectArgs> records, uint32_t drawn, Image image)
    {
        for (uint32_t i = 0; i < drawn; ++i)
        {
            const DrawMeshTasksIndirectArgs& record = records[i];
            for (uint32_t z = 0; z < record.z; ++z)
            {
                for (uint32_t y = 0; y < record.y; ++y)
                {
                    for (uint32_t group = 0; group < record.x; ++group)
                    {
                        image.draw(record.drawId, group, y, z);
                    }
                }
            }
        }
        return image;
    }

    //==================================================================================================================
    /// The format of an oracle target for drawIds below `maxCount`: `RGBA16Float`, which also shows the group, while it holds
    /// every value exactly, else `R32Float`, which holds the drawId alone; the cell still names the group.
    Format formatFor(uint32_t maxCount)
    {
        return maxCount <= kHalfExact ? Format::RGBA16Float : Format::R32Float;
    }
} // namespace

//======================================================================================================================
TEST_CASE("a lone mesh record draws with its drawId", "[conformance]")
{
    test::requireGpu();
    test::GpuKit kit;
    requireMeshTasksIndirect(kit.device);
    const Oracle oracle(kit);
    const Format format = Format::RGBA16Float;
    const Pipeline pipeline = oracle.pipeline(format);
    const Texture target = oracle.target(format);
    const std::vector<DrawMeshTasksIndirectArgs> records = meshRecords(1, 77);
    const Buffer args = kit.buffer(sizeof(DrawMeshTasksIndirectArgs), Memory::Gpu, BufferUsage::IndirectArgs, bytesOf(std::span(records)));
    CommandList list = kit.list(QueueType::Graphics, "test.mesh");
    Oracle::begin(list, target, pipeline);
    list.drawMeshTasksIndirect(kit.device.address(args), 1);
    list.endRenderPass();
    kit.run(std::array{ list });
    const std::string difference = differences(oracle.read(target, format), drawnImage(records, 1, Image(format)));
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("drawMeshTasksIndirect draws min(count, maxCount) records of groups varying in x, y and z", "[conformance]")
{
    test::requireGpu();
    const uint32_t maxCount = GENERATE(1u, 16u, 256u, 4096u);
    const auto kind = GENERATE(CountKind::Null, CountKind::Full, CountKind::Quarter, CountKind::Zero, CountKind::Above);
    CAPTURE(maxCount, nameOf(kind));
    test::GpuKit kit;
    requireMeshTasksIndirect(kit.device);
    const Oracle oracle(kit);
    const Format format = formatFor(maxCount);
    const Pipeline pipeline = oracle.pipeline(format);
    const Texture target = oracle.target(format);
    const std::vector<DrawMeshTasksIndirectArgs> records = meshRecords(maxCount, 0, true);
    const Buffer args = kit.buffer(records.size() * sizeof(DrawMeshTasksIndirectArgs), Memory::Gpu, BufferUsage::IndirectArgs, bytesOf(std::span(records)));
    const uint32_t count = countOf(kind, maxCount);
    const Buffer counted = kind == CountKind::Null ? Buffer{} : oracle.count(count);
    CommandList list = kit.list(QueueType::Graphics, "test.mesh");
    Oracle::begin(list, target, pipeline);
    list.drawMeshTasksIndirect(kit.device.address(args), maxCount, kind == CountKind::Null ? Addr{} : kit.device.address(counted));
    list.endRenderPass();
    kit.run(std::array{ list });
    const std::string difference = differences(oracle.read(target, format), drawnImage(records, std::min(count, maxCount), Image(format)));
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("mesh records written by the GPU earlier in the list are read after the barrier", "[conformance]")
{
    test::requireGpu();
    // A lone record, the writer kernel and an indirect command buffer with and without a count.
    const uint32_t maxCount = GENERATE(1u, 64u);
    const bool withCount = GENERATE(false, true);
    CAPTURE(maxCount, withCount);
    test::GpuKit kit;
    requireMeshTasksIndirect(kit.device);
    const Oracle oracle(kit);
    const Format format = Format::RGBA16Float;
    const Pipeline pipeline = oracle.pipeline(format);
    const Texture target = oracle.target(format);
    const std::vector<DrawMeshTasksIndirectArgs> records = meshRecords(maxCount);
    const uint64_t bytes = records.size() * sizeof(DrawMeshTasksIndirectArgs);
    const Buffer staged = kit.buffer(bytes, Memory::Upload, BufferUsage::None, bytesOf(std::span(records)));
    // The records and the count start as zeroes, which draw nothing: only the GPU's writes make them draw.
    const std::vector<std::byte> zeroes(bytes, std::byte{ 0 });
    const Buffer args = kit.buffer(bytes, Memory::Gpu, BufferUsage::IndirectArgs, zeroes);
    // The fill kernel writes the count, so its buffer takes shader writes.
    const uint32_t zero = 0;
    const Buffer counted = kit.buffer(sizeof(zero), Memory::Gpu, BufferUsage::IndirectArgs | BufferUsage::ShaderWrite, std::as_bytes(std::span(&zero, 1)));
    const uint32_t count = maxCount == 1 ? 1 : 37;
    CommandList list = kit.list(QueueType::Graphics, "test.mesh");
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
    list.drawMeshTasksIndirect(kit.device.address(args), maxCount, withCount ? kit.device.address(counted) : Addr{});
    list.endRenderPass();
    kit.run(std::array{ list });
    const uint32_t drawn = withCount ? std::min(count, maxCount) : maxCount;
    const std::string difference = differences(oracle.read(target, format), drawnImage(records, drawn, Image(format)));
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("a direct mesh draw after a counted one in the same pass keeps drawId 0", "[conformance]")
{
    test::requireGpu();
    // A lone record, the writer kernel and an indirect command buffer, each before and after a direct draw.
    const uint32_t variant = GENERATE(0u, 1u, 2u);
    const uint32_t maxCount = variant == 2 ? 4 : 1;
    const bool withCount = variant != 0;
    CAPTURE(maxCount, withCount);
    test::GpuKit kit;
    requireMeshTasksIndirect(kit.device);
    const Oracle oracle(kit);
    const Format format = Format::RGBA16Float;
    const Pipeline pipeline = oracle.pipeline(format);
    const Texture target = oracle.target(format);
    const std::vector<DrawMeshTasksIndirectArgs> before = meshRecords(maxCount, 8);
    const std::vector<DrawMeshTasksIndirectArgs> after = meshRecords(maxCount, 40);
    const uint64_t bytes = maxCount * sizeof(DrawMeshTasksIndirectArgs);
    const Buffer beforeArgs = kit.buffer(bytes, Memory::Gpu, BufferUsage::IndirectArgs, bytesOf(std::span(before)));
    const Buffer afterArgs = kit.buffer(bytes, Memory::Gpu, BufferUsage::IndirectArgs, bytesOf(std::span(after)));
    const Buffer counted = oracle.count(maxCount);
    const Addr count = withCount ? kit.device.address(counted) : Addr{};
    CommandList list = kit.list(QueueType::Graphics, "test.mesh");
    Oracle::begin(list, target, pipeline);
    list.drawMeshTasksIndirect(kit.device.address(beforeArgs), maxCount, count);
    // The direct draws read the zero record, not the record bound for the draw before them.
    list.drawMeshTasks(1, 1, 1);
    list.drawMeshTasksIndirect(kit.device.address(afterArgs), maxCount, count);
    list.drawMeshTasks(1, 2, 1);
    list.endRenderPass();
    kit.run(std::array{ list });
    Image expected = drawnImage(before, maxCount, Image(format));
    expected = drawnImage(after, maxCount, expected);
    // The direct draws' task groups have group.x 0, so both land on cell 0; the second's group with y 1 lands a plane below.
    expected.draw(0, 0);
    expected.draw(0, 0, 1);
    const std::string difference = differences(oracle.read(target, format), expected);
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("each stage of an indirect mesh draw reads the root recorded before it and the view and sampler it names", "[conformance]")
{
    test::requireGpu();
    // A lone record, the writer kernel, and indirect command buffers with and without a count, whose commands bind the four
    // reserved buffers themselves.
    const uint32_t variant = GENERATE(0u, 1u, 2u, 3u);
    const uint32_t maxCount = variant >= 2 ? 16 : 1;
    const bool withCount = variant == 1 || variant == 2;
    CAPTURE(maxCount, withCount);
    constexpr uint32_t kTexels = 16;
    constexpr uint32_t kTexelBase = 100;
    constexpr MeshRoot kRoot{ .taskBias = 500, .meshShift = 1000, .fragmentBias = 40, .pad = {} };
    test::GpuKit kit;
    requireMeshTasksIndirect(kit.device);
    const std::vector<std::byte> blob = test::shaderLibraryFile("MeshRootOracle");
    const ShaderLibrary library = *kit.device.loadShaderLibrary(blob, "MeshRootOracle");
    const Format format = Format::RGBA16Float;
    const std::array targets{ ColorTarget{ .format = format } };
    const Pipeline pipeline = *kit.device.createPipeline(
        GraphicsPipelineDesc{ .task = { library, "rootTaskMain" },
                              .mesh = { library, "rootMeshMain" },
                              .fragment = { library, "rootFragmentMain" },
                              .colors = targets,
                              .label = "test.meshRootOracle" }
    );
    std::array<float, kTexels> row{};
    for (uint32_t i = 0; i < kTexels; ++i)
    {
        row[i] = static_cast<float>(kTexelBase + i);
    }
    const std::array data{ TextureData{ std::as_bytes(std::span(row)), sizeof(row), 0 } };
    const Texture sampled = *kit.device.createTexture(
        TextureDesc{ .format = Format::R32Float, .extent = { kTexels, 1, 1 }, .usage = TextureUsage::Sampled, .label = "test.row" }, data
    );
    const ViewIndex view = *kit.device.createView(sampled, {});
    const SamplerIndex sampler =
        *kit.device.createSampler(SamplerDesc{ .filter = SamplerFilter::Nearest, .mipFilter = SamplerFilter::Nearest, .address = SamplerAddress::ClampToEdge });
    MeshRoot root = kRoot;
    root.view = static_cast<uint32_t>(view);
    root.sampler = static_cast<uint32_t>(sampler);
    // A second root for a second draw, whose values and cells all differ from the first's.
    MeshRoot other = root;
    other.taskBias = 600;
    other.meshShift = 3000;
    other.fragmentBias = 50;
    const Texture target =
        *kit.device.createTexture(TextureDesc{ .format = format, .extent = { kEdge, kEdge, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" });
    // drawIds 5 to 20 for the first draw and 25 to 40 for the second, so the texel each draw samples differs from its drawId
    // and a draw that took the other's root shows it.
    const std::array<std::vector<DrawMeshTasksIndirectArgs>, 2> records{ meshRecords(maxCount, 5), meshRecords(maxCount, 25) };
    const uint64_t bytes = maxCount * sizeof(DrawMeshTasksIndirectArgs);
    const std::array args{ kit.buffer(bytes, Memory::Gpu, BufferUsage::IndirectArgs, bytesOf(std::span(records[0]))),
                           kit.buffer(bytes, Memory::Gpu, BufferUsage::IndirectArgs, bytesOf(std::span(records[1]))) };
    const std::array<MeshRoot, 2> roots{ root, other };
    const uint32_t value = maxCount;
    const Buffer counted = kit.buffer(sizeof(value), Memory::Gpu, BufferUsage::IndirectArgs, std::as_bytes(std::span(&value, 1)));
    CommandList list = kit.list(QueueType::Graphics, "test.mesh");
    // A root bound before the pass, which the draw must not see: the one recorded inside the pass replaces it.
    list.pushRoot(MeshRoot{ .view = root.view, .sampler = root.sampler, .taskBias = 7, .meshShift = 3, .fragmentBias = 9, .pad = {} });
    const std::array colors{ ColorAttachment{ .texture = target, .load = LoadOp::Clear } };
    list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = kFullRect, .label = "test.pass" });
    list.setPipeline(pipeline);
    list.setViewport(kFull);
    list.setScissor(kFullRect);
    for (size_t i = 0; i < roots.size(); ++i)
    {
        list.pushRoot(roots[i]);
        list.drawMeshTasksIndirect(kit.device.address(args[i]), maxCount, withCount ? kit.device.address(counted) : Addr{});
    }
    list.endRenderPass();
    kit.run(std::array{ list });
    Image expected(format);
    for (size_t i = 0; i < roots.size(); ++i)
    {
        const MeshRoot& bound = roots[i];
        for (const DrawMeshTasksIndirectArgs& record : records[i])
        {
            for (uint32_t group = 0; group < record.x; ++group)
            {
                expected.set(
                    record.drawId + bound.meshShift + kGroupCells * group,
                    { static_cast<float>(record.drawId + 1),
                      static_cast<float>(bound.taskBias + kTexelBase + record.drawId % kTexels),
                      static_cast<float>(group + 1 + bound.fragmentBias),
                      1.0f }
                );
            }
        }
    }
    const std::string difference = differences(kit.readTexture(target, TextureRegion{ .extent = { kEdge, kEdge, 1 } }, format), expected);
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("a mesh pipeline without a task entry draws its counted records", "[conformance]")
{
    test::requireGpu();
    // The writer kernel, and indirect command buffers with and without a count.
    const uint32_t variant = GENERATE(0u, 1u, 2u);
    const uint32_t maxCount = variant == 0 ? 1 : 16;
    const bool withCount = variant == 2;
    CAPTURE(maxCount, withCount);
    test::GpuKit kit;
    requireMeshTasksIndirect(kit.device);
    const Oracle oracle(kit);
    const Format format = Format::RGBA16Float;
    const Pipeline pipeline = oracle.pipeline(format, true);
    const Texture target = oracle.target(format);
    const std::vector<DrawMeshTasksIndirectArgs> records = meshRecords(maxCount, 30, true);
    const Buffer args = kit.buffer(records.size() * sizeof(DrawMeshTasksIndirectArgs), Memory::Gpu, BufferUsage::IndirectArgs, bytesOf(std::span(records)));
    const uint32_t count = maxCount / 2 + 1;
    const Buffer counted = oracle.count(count);
    CommandList list = kit.list(QueueType::Graphics, "test.mesh");
    Oracle::begin(list, target, pipeline);
    list.drawMeshTasksIndirect(kit.device.address(args), maxCount, withCount ? kit.device.address(counted) : Addr{});
    list.endRenderPass();
    kit.run(std::array{ list });
    const std::string difference = differences(oracle.read(target, format), drawnImage(records, withCount ? count : maxCount, Image(format)));
    INFO(difference);
    CHECK(difference.empty());
}

//======================================================================================================================
TEST_CASE("a mesh record with no groups in x, y or z draws nothing where the list drew one before", "[conformance]")
{
    test::requireGpu();
    // Two frame slots, so the third frame acquires the list of the first, whose indirect command buffer its counted draw
    // already filled with every record.
    constexpr uint32_t kSlots = 2;
    test::GpuKit kit(kSlots);
    requireMeshTasksIndirect(kit.device);
    const Oracle oracle(kit);
    const Format format = Format::RGBA16Float;
    const Pipeline pipeline = oracle.pipeline(format);
    constexpr uint32_t kRecords = 16;
    const std::vector<DrawMeshTasksIndirectArgs> drawn = meshRecords(kRecords);
    // In the third frame, records 1 and 7 have no groups in x, record 3 none in y and record 5 none in z.
    std::vector<DrawMeshTasksIndirectArgs> emptied = drawn;
    for (uint32_t i = 1; i < 8; i += 2)
    {
        (i % 6 == 1 ? emptied[i].x : i % 6 == 3 ? emptied[i].y : emptied[i].z) = 0;
    }
    const auto record = [&](Texture target, std::span<const DrawMeshTasksIndirectArgs> records) {
        const Buffer args = kit.buffer(records.size() * sizeof(DrawMeshTasksIndirectArgs), Memory::Gpu, BufferUsage::IndirectArgs, bytesOf(records));
        CommandList list = kit.list(QueueType::Graphics, "test.mesh");
        Oracle::begin(list, target, pipeline);
        list.drawMeshTasksIndirect(kit.device.address(args), kRecords);
        list.endRenderPass();
        kit.run(std::array{ list });
    };
    record(oracle.target(format), drawn);
    for (uint32_t frame = 0; frame < kSlots; ++frame)
    {
        kit.device.endFrame();
        kit.device.beginFrame();
    }
    const Texture target = oracle.target(format);
    record(target, emptied);
    const std::string difference = differences(oracle.read(target, format), drawnImage(emptied, kRecords, Image(format)));
    INFO(difference);
    CHECK(difference.empty());
}
