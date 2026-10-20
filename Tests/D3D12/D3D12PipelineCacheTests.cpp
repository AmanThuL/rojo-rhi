//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12PipelineCacheTests.cpp
/// @brief Tests the D3D12 pipeline cache: its key, its blob, hits and misses across devices, refused blobs and threads.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "D3D12PipelineKey.h"
#include "D3D12ShaderLibrary.h"
#include "ShaderList.h"
#include "Support/Backend.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <format>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The number of lines of a blob's header.
    constexpr size_t kHeaderLines = 4;

    /// The one colour target the render pipelines draw to.
    constexpr std::array<ColorTarget, 1> kColor = { ColorTarget{ .format = Format::RGBA8Unorm } };

    //==================================================================================================================
    /// Returns the bytes of `text`.
    std::vector<std::byte> bytesOf(std::string_view text)
    {
        const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
        return std::vector<std::byte>(bytes, bytes + text.size());
    }

    //==================================================================================================================
    /// Returns 24 bytes standing in for a DXIL container: `DXBC`, a 16-byte digest of `seed` repeated, then four bytes of
    /// `tail`, which lie past the digest.
    std::array<std::byte, 24> fakeDxil(uint8_t seed, uint8_t tail = 0)
    {
        std::array<std::byte, 24> bytes = {};
        bytes[0] = std::byte{ 'D' };
        bytes[1] = std::byte{ 'X' };
        bytes[2] = std::byte{ 'B' };
        bytes[3] = std::byte{ 'C' };
        std::fill(bytes.begin() + 4, bytes.begin() + 20, std::byte{ seed });
        std::fill(bytes.begin() + 20, bytes.end(), std::byte{ tail });
        return bytes;
    }

    /// The stand-in DXIL of each stage of the key cases.
    const std::array<std::byte, 24> kVertexDxil = fakeDxil(1);
    const std::array<std::byte, 24> kFragmentDxil = fakeDxil(2);
    const std::array<std::byte, 24> kOtherDxil = fakeDxil(3);
    const std::array<std::byte, 24> kVertexTailDxil = fakeDxil(1, 9);

    /// One graphics description of the key cases, with storage for its colour targets.
    struct KeyVariant
    {
        GraphicsPipelineDesc desc;         ///< The description; `colors` is set from the fields below.
        std::array<ColorTarget, 2> colors; ///< The colour targets' storage.
        size_t colorCount = 1;             ///< The colour targets in use.
        d3d12::StageDxil dxil;             ///< The stages' DXIL.
    };

    //==================================================================================================================
    /// Returns the key of `variant`.
    std::array<uint64_t, 2> keyOf(KeyVariant variant)
    {
        variant.desc.colors = std::span<const ColorTarget>(variant.colors).first(variant.colorCount);
        return d3d12::pipelineKey(d3d12::canonicalKey(variant.desc, variant.dxil));
    }

    //==================================================================================================================
    /// The base vertex and fragment pipeline of the key cases.
    KeyVariant baseVariant()
    {
        KeyVariant variant;
        variant.desc.vertex = { ShaderLibrary{ 1 }, "vsMain" };
        variant.desc.fragment = { ShaderLibrary{ 1 }, "fsMain" };
        variant.desc.label = "test.key";
        variant.colors[0] = ColorTarget{ .format = Format::RGBA8Unorm };
        variant.dxil = d3d12::StageDxil{ .vertex = kVertexDxil, .fragment = kFragmentDxil };
        return variant;
    }

    /// One single-field edit of the base pipeline.
    struct KeyEdit
    {
        std::string_view name;              ///< What the edit changes.
        void (*apply)(KeyVariant& variant); ///< Makes the change.
    };

    /// Every single-field edit of the base pipeline, each of which reaches the pipeline state stream.
    const std::array<KeyEdit, 39> kEdits = { {
        { "primitive", [](KeyVariant& v) { v.desc.primitive = PrimitiveClass::Line; } },
        { "raster.cull", [](KeyVariant& v) { v.desc.raster.cull = CullMode::Back; } },
        { "raster.frontFace", [](KeyVariant& v) { v.desc.raster.frontFace = FrontFace::Clockwise; } },
        { "raster.fill", [](KeyVariant& v) { v.desc.raster.fill = FillMode::Wireframe; } },
        { "raster.depthClip", [](KeyVariant& v) { v.desc.raster.depthClip = false; } },
        { "raster.depthBias", [](KeyVariant& v) { v.desc.raster.depthBias = true; } },
        { "raster.conservative", [](KeyVariant& v) { v.desc.raster.conservative = true; } },
        { "depthStencil.depthTest", [](KeyVariant& v) { v.desc.depthStencil.depthTest = true; } },
        { "depthStencil.depthWrite", [](KeyVariant& v) { v.desc.depthStencil.depthWrite = true; } },
        { "depthStencil.depthCompare", [](KeyVariant& v) { v.desc.depthStencil.depthCompare = CompareOp::Less; } },
        { "depthStencil.stencilTest", [](KeyVariant& v) { v.desc.depthStencil.stencilTest = true; } },
        { "depthStencil.stencilReadMask", [](KeyVariant& v) { v.desc.depthStencil.stencilReadMask = 0x0F; } },
        { "depthStencil.stencilWriteMask", [](KeyVariant& v) { v.desc.depthStencil.stencilWriteMask = 0xF0; } },
        { "depthStencil.front.fail", [](KeyVariant& v) { v.desc.depthStencil.front.fail = StencilOp::Zero; } },
        { "depthStencil.front.depthFail", [](KeyVariant& v) { v.desc.depthStencil.front.depthFail = StencilOp::Replace; } },
        { "depthStencil.front.pass", [](KeyVariant& v) { v.desc.depthStencil.front.pass = StencilOp::Invert; } },
        { "depthStencil.front.compare", [](KeyVariant& v) { v.desc.depthStencil.front.compare = CompareOp::Never; } },
        { "depthStencil.back.fail", [](KeyVariant& v) { v.desc.depthStencil.back.fail = StencilOp::Zero; } },
        { "depthStencil.back.depthFail", [](KeyVariant& v) { v.desc.depthStencil.back.depthFail = StencilOp::Replace; } },
        { "depthStencil.back.pass", [](KeyVariant& v) { v.desc.depthStencil.back.pass = StencilOp::Invert; } },
        { "depthStencil.back.compare", [](KeyVariant& v) { v.desc.depthStencil.back.compare = CompareOp::Never; } },
        { "colors[0].format", [](KeyVariant& v) { v.colors[0].format = Format::RGBA16Float; } },
        { "colors[0].blend.enable", [](KeyVariant& v) { v.colors[0].blend.enable = true; } },
        { "colors[0].blend.srcColor", [](KeyVariant& v) { v.colors[0].blend.srcColor = BlendFactor::SrcAlpha; } },
        { "colors[0].blend.dstColor", [](KeyVariant& v) { v.colors[0].blend.dstColor = BlendFactor::OneMinusSrcAlpha; } },
        { "colors[0].blend.colorOp", [](KeyVariant& v) { v.colors[0].blend.colorOp = BlendOp::Max; } },
        { "colors[0].blend.srcAlpha", [](KeyVariant& v) { v.colors[0].blend.srcAlpha = BlendFactor::Zero; } },
        { "colors[0].blend.dstAlpha", [](KeyVariant& v) { v.colors[0].blend.dstAlpha = BlendFactor::One; } },
        { "colors[0].blend.alphaOp", [](KeyVariant& v) { v.colors[0].blend.alphaOp = BlendOp::Subtract; } },
        { "colors[0].writeMask", [](KeyVariant& v) { v.colors[0].writeMask = ColorWriteMask::R; } },
        { "a second colour target", [](KeyVariant& v) { v.colorCount = 2; } },
        { "no colour target", [](KeyVariant& v) { v.colorCount = 0; } },
        { "depthFormat", [](KeyVariant& v) { v.desc.depthFormat = Format::D32Float; } },
        { "samples", [](KeyVariant& v) { v.desc.samples = 4; } },
        { "vertex entry name", [](KeyVariant& v) { v.desc.vertex.name = "vsOther"; } },
        { "fragment entry name", [](KeyVariant& v) { v.desc.fragment.name = "fsOther"; } },
        { "vertex DXIL", [](KeyVariant& v) { v.dxil.vertex = kOtherDxil; } },
        { "fragment DXIL", [](KeyVariant& v) { v.dxil.fragment = kOtherDxil; } },
        { "no fragment entry",
          [](KeyVariant& v) {
              v.desc.fragment = {};
              v.dxil.fragment = {};
          } },
    } };

    //==================================================================================================================
    /// Returns a DXIL container holding a `SFI0` part and, with `psv`, a `PSV0` part whose runtime information is
    /// `infoSize` bytes with the shader kind `kind` at byte 24 and the mesh output topology `topology` at byte 27.
    std::vector<std::byte> psvContainer(uint8_t kind, uint8_t topology, uint32_t infoSize = 52, bool psv = true)
    {
        const auto le32 = [](std::vector<std::byte>& bytes, size_t at, uint32_t value) {
            for (size_t i = 0; i < 4; ++i)
            {
                bytes[at + i] = static_cast<std::byte>((value >> (8 * i)) & 0xFF);
            }
        };
        std::vector<std::vector<std::byte>> parts;
        std::vector<std::byte> features(8 + 8);
        features[0] = std::byte{ 'S' };
        features[1] = std::byte{ 'F' };
        features[2] = std::byte{ 'I' };
        features[3] = std::byte{ '0' };
        le32(features, 4, 8);
        parts.push_back(features);
        if (psv)
        {
            std::vector<std::byte> part(8 + 4 + infoSize);
            part[0] = std::byte{ 'P' };
            part[1] = std::byte{ 'S' };
            part[2] = std::byte{ 'V' };
            part[3] = std::byte{ '0' };
            le32(part, 4, 4 + infoSize);
            le32(part, 8, infoSize);
            if (infoSize > 27)
            {
                part[12 + 24] = std::byte{ kind };
                part[12 + 27] = std::byte{ topology };
            }
            parts.push_back(part);
        }
        std::vector<std::byte> bytes(32 + 4 * parts.size());
        bytes[0] = std::byte{ 'D' };
        bytes[1] = std::byte{ 'X' };
        bytes[2] = std::byte{ 'B' };
        bytes[3] = std::byte{ 'C' };
        le32(bytes, 28, static_cast<uint32_t>(parts.size()));
        for (size_t i = 0; i < parts.size(); ++i)
        {
            le32(bytes, 32 + 4 * i, static_cast<uint32_t>(bytes.size()));
            bytes.insert(bytes.end(), parts[i].begin(), parts[i].end());
        }
        le32(bytes, 24, static_cast<uint32_t>(bytes.size()));
        return bytes;
    }

    std::mutex gMessageMutex;               ///< Guards `gMessages`; the callback may run on any thread.
    std::vector<MessageSeverity> gMessages; ///< The severities of the messages `messagesOf` collected.

    //==================================================================================================================
    /// Collects the severity of every message delivered to the message callback.
    void collectMessage(MessageSeverity severity, std::string_view, void*)
    {
        const std::lock_guard lock(gMessageMutex);
        gMessages.push_back(severity);
    }

    //==================================================================================================================
    /// Loads the standard library on `device`.
    ShaderLibrary loadStandard(Device& device)
    {
        const Result<ShaderLibrary> library = device.loadShaderLibrary(test::standardLibrary(), "test.standard");
        REQUIRE(library.has_value());
        return *library;
    }

    //==================================================================================================================
    /// Creates the standard vertex and fragment pipeline, failing the case when creation fails.
    Pipeline createRender(Device& device, ShaderLibrary library)
    {
        const Result<Pipeline> pipeline = device.createPipeline(
            GraphicsPipelineDesc{ .vertex = { library, "vsMain" }, .fragment = { library, "fsMain" }, .colors = kColor, .label = "test.render" }
        );
        INFO((pipeline ? std::string() : pipeline.error().message));
        REQUIRE(pipeline.has_value());
        return *pipeline;
    }

    //==================================================================================================================
    /// Creates the standard compute pipeline, failing the case when creation fails.
    Pipeline createCompute(Device& device, ShaderLibrary library)
    {
        const Result<Pipeline> pipeline = device.createPipeline(ComputePipelineDesc{ .compute = { library, "csMain" }, .label = "test.compute" });
        INFO((pipeline ? std::string() : pipeline.error().message));
        REQUIRE(pipeline.has_value());
        return *pipeline;
    }

    //==================================================================================================================
    /// Creates the task, mesh and fragment pipeline of the packaged `AbiMesh` library, failing the case when creation fails.
    Pipeline createMesh(Device& device)
    {
        const Result<ShaderLibrary> library = device.loadShaderLibrary(test::shaderLibraryFile("AbiMesh"), "test.AbiMesh");
        REQUIRE(library.has_value());
        const Result<Pipeline> pipeline = device.createPipeline(
            GraphicsPipelineDesc{ .task = { *library, "taskMain" }, .mesh = { *library, "meshMain" }, .colors = kColor, .label = "test.mesh" }
        );
        INFO((pipeline ? std::string() : pipeline.error().message));
        REQUIRE(pipeline.has_value());
        return *pipeline;
    }

    //==================================================================================================================
    /// Returns the cache blob of `device`, failing the case when it cannot be serialized.
    std::vector<std::byte> harvest(Device& device)
    {
        Result<std::vector<std::byte>> data = device.pipelineCacheData();
        INFO((data ? std::string() : data.error().message));
        REQUIRE(data.has_value());
        return std::move(*data);
    }

    //==================================================================================================================
    /// Creates a device with `blob` as its pipeline cache, failing the case when creation fails, and writes the severities of
    /// the messages its creation delivered to `messages`.
    Device deviceWith(std::span<const std::byte> blob, std::vector<MessageSeverity>& messages)
    {
        {
            const std::lock_guard lock(gMessageMutex);
            gMessages.clear();
        }
        setMessageCallback(collectMessage, nullptr);
        Result<Device> device = createDevice(DeviceDesc{ .framesInFlight = 2, .pipelineCache = blob });
        setMessageCallback(nullptr, nullptr);
        INFO((device ? std::string() : device.error().message));
        REQUIRE(device.has_value());
        const std::lock_guard lock(gMessageMutex);
        messages = gMessages;
        return *device;
    }

    //==================================================================================================================
    /// The blob of a device that created the standard render pipeline.
    std::vector<std::byte> renderBlob()
    {
        std::vector<MessageSeverity> messages;
        Device device = deviceWith({}, messages);
        createRender(device, loadStandard(device));
        std::vector<std::byte> blob = harvest(device);
        destroyDevice(device);
        return blob;
    }

    //==================================================================================================================
    /// The header lines of `blob`, without their line feeds, and the offset of the bytes after them.
    std::pair<std::vector<std::string>, size_t> headerOf(const std::vector<std::byte>& blob)
    {
        const std::string_view text(reinterpret_cast<const char*>(blob.data()), blob.size());
        std::vector<std::string> lines;
        size_t at = 0;
        while (lines.size() < kHeaderLines)
        {
            const size_t end = text.find('\n', at);
            REQUIRE(end != std::string_view::npos);
            lines.emplace_back(text.substr(at, end - at));
            at = end + 1;
        }
        return { lines, at };
    }

    //==================================================================================================================
    /// Returns `lines`, each followed by a line feed, and then `payload`.
    std::vector<std::byte> joined(const std::vector<std::string>& lines, std::span<const std::byte> payload)
    {
        std::string header;
        for (const std::string& line : lines)
        {
            header += line;
            header += '\n';
        }
        std::vector<std::byte> blob = bytesOf(header);
        blob.insert(blob.end(), payload.begin(), payload.end());
        return blob;
    }

    //==================================================================================================================
    /// Returns `blob` with header line `index` replaced by `line`.
    std::vector<std::byte> withLine(const std::vector<std::byte>& blob, size_t index, std::string line)
    {
        auto [lines, at] = headerOf(blob);
        lines[index] = std::move(line);
        return joined(lines, std::span(blob).subspan(at));
    }
} // namespace

//======================================================================================================================
TEST_CASE("pipelineKey of the empty input is the FNV-1a 128 offset basis", "[d3d12]")
{
    const std::array<uint64_t, 2> key = d3d12::pipelineKey({});
    CHECK(key[0] == 0x6c62272e07bb0142);
    CHECK(key[1] == 0x62b821756295c58d);
    CHECK(d3d12::keyName(key) == L"6c62272e07bb014262b821756295c58d");
}

//======================================================================================================================
TEST_CASE("pipelineKey of \"a\" and of \"foobar\" match FNV-1a 128", "[d3d12]")
{
    CHECK(d3d12::keyName(d3d12::pipelineKey(bytesOf("a"))) == L"d228cb696f1a8caf78912b704e4a8964");
    CHECK(d3d12::keyName(d3d12::pipelineKey(bytesOf("foobar"))) == L"343e1662793c64bf6f0d3597ba446f18");
    CHECK(d3d12::keyName({ 0, 1 }) == L"00000000000000000000000000000001");
}

//======================================================================================================================
TEST_CASE("equal descriptions give one key; changing any stream field or a stage's DXIL changes it", "[d3d12]")
{
    const std::array<uint64_t, 2> base = keyOf(baseVariant());
    CHECK(keyOf(baseVariant()) == base);

    // Neither the label nor DXIL bytes past the digest reach the key.
    KeyVariant relabelled = baseVariant();
    relabelled.desc.label = "test.other";
    relabelled.dxil.vertex = kVertexTailDxil;
    CHECK(keyOf(relabelled) == base);

    std::vector<std::array<uint64_t, 2>> keys = { base };
    for (const KeyEdit& edit : kEdits)
    {
        INFO(edit.name);
        KeyVariant variant = baseVariant();
        edit.apply(variant);
        const std::array<uint64_t, 2> key = keyOf(variant);
        CHECK(key != base);
        keys.push_back(key);
    }
    std::ranges::sort(keys);
    CHECK(std::ranges::adjacent_find(keys) == keys.end());
}

//======================================================================================================================
TEST_CASE("a mesh pipeline's key leaves out the class, and a compute key differs from every graphics key", "[d3d12]")
{
    KeyVariant mesh = baseVariant();
    mesh.desc.vertex = {};
    mesh.desc.task = { ShaderLibrary{ 1 }, "tsMain" };
    mesh.desc.mesh = { ShaderLibrary{ 1 }, "msMain" };
    mesh.dxil = d3d12::StageDxil{ .task = kOtherDxil, .mesh = kVertexDxil, .fragment = kFragmentDxil };
    const std::array<uint64_t, 2> meshKey = keyOf(mesh);
    CHECK(meshKey != keyOf(baseVariant()));
    mesh.desc.primitive = PrimitiveClass::Point;
    CHECK(keyOf(mesh) == meshKey);
    mesh.dxil.task = {};
    mesh.desc.task = {};
    CHECK(keyOf(mesh) != meshKey);

    const ComputePipelineDesc compute{ .compute = { ShaderLibrary{ 1 }, "csMain" }, .label = "test.compute" };
    const std::array<uint64_t, 2> computeKey = d3d12::pipelineKey(d3d12::canonicalKey(compute, kVertexDxil));
    CHECK(computeKey != meshKey);
    CHECK(computeKey != keyOf(baseVariant()));
    CHECK(d3d12::pipelineKey(d3d12::canonicalKey(compute, kOtherDxil)) != computeKey);
    const ComputePipelineDesc renamed{ .compute = { ShaderLibrary{ 1 }, "csOther" }, .label = "test.compute" };
    CHECK(d3d12::pipelineKey(d3d12::canonicalKey(renamed, kVertexDxil)) != computeKey);
}

//======================================================================================================================
TEST_CASE("libraryOf reads the header blobHeader writes and names the reason it refuses another", "[d3d12]")
{
    const std::string header = d3d12::blobHeader(3);
    CHECK(
        header ==
        std::format(
            "rojorhi.d3d12.pipelinecache\nversion 1\nrojorhi {}.{}.{}\nlibrary 3\n", ROJORHI_VERSION_MAJOR, ROJORHI_VERSION_MINOR, ROJORHI_VERSION_PATCH
        )
    );
    const std::vector<std::byte> blob = bytesOf(header + "abc");
    const auto library = d3d12::libraryOf(blob);
    REQUIRE(library.has_value());
    CHECK(library->data() == blob.data() + header.size());
    CHECK(library->size() == 3);
    const auto empty = d3d12::libraryOf(bytesOf(d3d12::blobHeader(0)));
    REQUIRE(empty.has_value());
    CHECK(empty->empty());

    const std::string version = std::format("rojorhi {}.{}.{}\n", ROJORHI_VERSION_MAJOR, ROJORHI_VERSION_MINOR, ROJORHI_VERSION_PATCH);
    const std::vector<std::pair<std::string, std::string_view>> refused = {
        { "", "was not written by the D3D12 backend" },
        { "\x01\x02\x03\x04", "was not written by the D3D12 backend" },
        { "rojorhi.metal4.pipelinecache\nversion 1\n", "was not written by the D3D12 backend" },
        { "rojorhi.d3d12.pipelinecache", "was not written by the D3D12 backend" },
        { "rojorhi.d3d12.pipelinecache\nversion 2\n" + version + "library 0\n", "has another blob version" },
        { "rojorhi.d3d12.pipelinecache\nversion 1\nrojorhi 99.0.0\nlibrary 0\n", "was written by another RojoRHI version" },
        { "rojorhi.d3d12.pipelinecache\nversion 1\n" + version, "has a library line that does not count the bytes after it" },
        { "rojorhi.d3d12.pipelinecache\nversion 1\n" + version + "library 4\nabc", "has a library line that does not count the bytes after it" },
        { "rojorhi.d3d12.pipelinecache\nversion 1\n" + version + "library 2\nabc", "has a library line that does not count the bytes after it" },
        { "rojorhi.d3d12.pipelinecache\nversion 1\n" + version + "library x\nabc", "has a library line that does not count the bytes after it" },
        { "rojorhi.d3d12.pipelinecache\nversion 1\n" + version + "library \n", "has a library line that does not count the bytes after it" },
        { "rojorhi.d3d12.pipelinecache\nversion 1\n" + version + "library 3 \nabc", "has a library line that does not count the bytes after it" },
        { "rojorhi.d3d12.pipelinecache\nversion 1\n" + version + "archive 3\nabc", "has a library line that does not count the bytes after it" },
    };
    for (const auto& [text, reason] : refused)
    {
        INFO(text);
        const auto result = d3d12::libraryOf(bytesOf(text));
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error() == reason);
    }
}

//======================================================================================================================
TEST_CASE("meshOutputTopology reads the mesh output topology from PSV0 and is Unknown for anything else", "[d3d12]")
{
    CHECK(d3d12::meshOutputTopology(psvContainer(13, 2)) == d3d12::MeshTopology::Triangle);
    CHECK(d3d12::meshOutputTopology(psvContainer(13, 1)) == d3d12::MeshTopology::Line);
    CHECK(d3d12::meshOutputTopology(psvContainer(13, 0)) == d3d12::MeshTopology::Unknown);
    CHECK(d3d12::meshOutputTopology(psvContainer(13, 3)) == d3d12::MeshTopology::Unknown);
    // A compute (5) or amplification (14) entry declares no mesh output.
    CHECK(d3d12::meshOutputTopology(psvContainer(5, 2)) == d3d12::MeshTopology::Unknown);
    CHECK(d3d12::meshOutputTopology(psvContainer(14, 2)) == d3d12::MeshTopology::Unknown);
    // Runtime information of version 0 ends before the shader kind.
    CHECK(d3d12::meshOutputTopology(psvContainer(13, 2, 24)) == d3d12::MeshTopology::Unknown);
    CHECK(d3d12::meshOutputTopology(psvContainer(13, 2, 52, false)) == d3d12::MeshTopology::Unknown);
    CHECK(d3d12::meshOutputTopology({}) == d3d12::MeshTopology::Unknown);

    std::vector<std::byte> truncated = psvContainer(13, 2);
    truncated.resize(truncated.size() - 20);
    CHECK(d3d12::meshOutputTopology(truncated) == d3d12::MeshTopology::Unknown);
    std::vector<std::byte> counted = psvContainer(13, 2);
    counted[28] = std::byte{ 0xFF };
    counted[29] = std::byte{ 0xFF };
    CHECK(d3d12::meshOutputTopology(counted) == d3d12::MeshTopology::Unknown);
    std::vector<std::byte> magic = psvContainer(13, 2);
    magic[0] = std::byte{ 'X' };
    CHECK(d3d12::meshOutputTopology(magic) == d3d12::MeshTopology::Unknown);
}

//======================================================================================================================
TEST_CASE("the packaged mesh entry declares triangles and its amplification entry no mesh output", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    const std::vector<std::byte> blob = test::shaderLibraryFile("AbiMesh");
    const Result<d3d12::LibraryHeader> header = d3d12::parseShaderLibrary(blob);
    REQUIRE(header.has_value());
    size_t checked = 0;
    for (const d3d12::LibraryEntry& entry : header->entries)
    {
        INFO(entry.name);
        if (entry.name == "meshMain")
        {
            CHECK(d3d12::meshOutputTopology(entry.dxil) == d3d12::MeshTopology::Triangle);
            ++checked;
        }
        else if (entry.name == "taskMain")
        {
            CHECK(d3d12::meshOutputTopology(entry.dxil) == d3d12::MeshTopology::Unknown);
            ++checked;
        }
    }
    CHECK(checked == 2);
}

//======================================================================================================================
TEST_CASE("a second device loaded from pipelineCacheData hits every pipeline the first created", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    std::vector<MessageSeverity> messages;
    Device first = deviceWith({}, messages);
    CHECK(messages.empty());
    const ShaderLibrary library = loadStandard(first);
    createRender(first, library);
    createCompute(first, library);
    createMesh(first);
    const Counters created = first.counters();
    CHECK(created.pipelinesCreated == 3);
    CHECK(created.pipelineCacheHits == 0);
    CHECK(created.pipelineCacheMisses == 3);
    const std::vector<std::byte> blob = harvest(first);
    destroyDevice(first);

    const auto [lines, at] = headerOf(blob);
    CHECK(lines[0] == "rojorhi.d3d12.pipelinecache");
    CHECK(lines[1] == "version 1");
    CHECK(lines[2] == std::format("rojorhi {}.{}.{}", ROJORHI_VERSION_MAJOR, ROJORHI_VERSION_MINOR, ROJORHI_VERSION_PATCH));
    CHECK(lines[3] == std::format("library {}", blob.size() - at));
    CHECK(blob.size() > at);

    Device second = deviceWith(blob, messages);
    CHECK(messages.empty());
    const ShaderLibrary again = loadStandard(second);
    // Each pipeline is checked on its own, so a miss names the pipeline that missed.
    const auto hitOf = [&second](auto create) {
        const uint64_t before = second.counters().pipelineCacheHits;
        create();
        return second.counters().pipelineCacheHits - before;
    };
    CHECK(hitOf([&] { createRender(second, again); }) == 1);
    CHECK(hitOf([&] { createCompute(second, again); }) == 1);
    CHECK(hitOf([&] { createMesh(second); }) == 1);
    const Counters counters = second.counters();
    CHECK(counters.pipelinesCreated == 3);
    CHECK(counters.pipelineCacheMisses == 0);
    destroyDevice(second);
}

//======================================================================================================================
TEST_CASE("a pipeline created twice on one device is a hit the second time, mesh pipelines included", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    std::vector<MessageSeverity> messages;
    Device device = deviceWith({}, messages);
    const ShaderLibrary library = loadStandard(device);
    const auto hitOf = [&device](auto create) {
        const uint64_t before = device.counters().pipelineCacheHits;
        create();
        return device.counters().pipelineCacheHits - before;
    };
    CHECK(hitOf([&] { createRender(device, library); }) == 0);
    CHECK(hitOf([&] { createRender(device, library); }) == 1);
    CHECK(hitOf([&] { createCompute(device, library); }) == 0);
    CHECK(hitOf([&] { createCompute(device, library); }) == 1);
    CHECK(hitOf([&] { createMesh(device); }) == 0);
    CHECK(hitOf([&] { createMesh(device); }) == 1);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE(
    "a blob of another backend, another RojoRHI version, a bad library count or garbage library bytes is one warning and every creation a miss", "[d3d12]"
)
{
    const std::vector<std::byte> blob = renderBlob();
    const auto [lines, at] = headerOf(blob);
    REQUIRE(lines[3] == std::format("library {}", blob.size() - at));
    const size_t libraryBytes = blob.size() - at;
    std::vector<std::byte> truncated = blob;
    truncated.resize(truncated.size() - libraryBytes / 2);
    const std::vector<std::byte> garbage(libraryBytes, std::byte{ 0xAB });

    const std::vector<std::pair<std::string, std::vector<std::byte>>> refused = {
        { "another backend", withLine(blob, 0, "rojorhi.metal4.pipelinecache") },
        { "another blob version", withLine(blob, 1, "version 2") },
        { "another RojoRHI version", withLine(blob, 2, "rojorhi 99.0.0") },
        { "a library count one too many", withLine(blob, 3, std::format("library {}", libraryBytes + 1)) },
        { "a library count one too few", withLine(blob, 3, std::format("library {}", libraryBytes - 1)) },
        { "a library truncated after a valid header", truncated },
        { "garbage library bytes", joined(lines, garbage) },
        { "garbage", bytesOf("\x01\x02\x03\x04") },
    };
    for (const auto& [name, bytes] : refused)
    {
        INFO(name);
        std::vector<MessageSeverity> messages;
        Device device = deviceWith(bytes, messages);
        REQUIRE(messages.size() == 1);
        CHECK(messages[0] == MessageSeverity::Warning);
        createRender(device, loadStandard(device));
        CHECK(device.counters().pipelineCacheHits == 0);
        CHECK(device.counters().pipelineCacheMisses == 1);
        const std::vector<std::byte> fresh = harvest(device);
        destroyDevice(device);

        // The device that discarded the blob still hands out one a later device loads cleanly.
        Device third = deviceWith(fresh, messages);
        CHECK(messages.empty());
        createRender(third, loadStandard(third));
        CHECK(third.counters().pipelineCacheHits == 1);
        CHECK(third.counters().pipelineCacheMisses == 0);
        destroyDevice(third);
    }
}

//======================================================================================================================
TEST_CASE("pipelineCacheData with no new pipeline returns a blob that loads with the same hits", "[d3d12]")
{
    std::vector<MessageSeverity> messages;
    Device first = deviceWith({}, messages);
    // A device that created nothing hands out a header and an empty library, which loads without a message.
    const std::vector<std::byte> idle = harvest(first);
    const ShaderLibrary library = loadStandard(first);
    createRender(first, library);
    createCompute(first, library);
    const std::vector<std::byte> blob = harvest(first);
    destroyDevice(first);

    Device empty = deviceWith(idle, messages);
    CHECK(messages.empty());
    createRender(empty, loadStandard(empty));
    CHECK(empty.counters().pipelineCacheHits == 0);
    destroyDevice(empty);

    Device second = deviceWith(blob, messages);
    CHECK(messages.empty());
    const ShaderLibrary again = loadStandard(second);
    createRender(second, again);
    createCompute(second, again);
    REQUIRE(second.counters().pipelineCacheHits == 2);
    const std::vector<std::byte> served = harvest(second);
    destroyDevice(second);

    Device third = deviceWith(served, messages);
    CHECK(messages.empty());
    const ShaderLibrary last = loadStandard(third);
    createRender(third, last);
    createCompute(third, last);
    CHECK(third.counters().pipelineCacheHits == 2);
    CHECK(third.counters().pipelineCacheMisses == 0);
    destroyDevice(third);
}

//======================================================================================================================
TEST_CASE("concurrent createPipeline calls on two threads each count a hit or a miss, never both", "[d3d12]")
{
    constexpr size_t kRounds = 8;
    std::vector<MessageSeverity> messages;
    Device device = deviceWith(renderBlob(), messages);
    CHECK(messages.empty());
    const ShaderLibrary library = loadStandard(device);
    std::atomic<size_t> failures = 0;
    const auto creator = [&] {
        for (size_t round = 0; round < kRounds; ++round)
        {
            // The render pipeline is in the blob; the compute pipeline is stored by whichever thread first misses it.
            const GraphicsPipelineDesc render{ .vertex = { library, "vsMain" }, .fragment = { library, "fsMain" }, .colors = kColor };
            if (!device.createPipeline(render).has_value())
            {
                ++failures;
            }
            if (!device.createPipeline(ComputePipelineDesc{ .compute = { library, "csMain" } }).has_value())
            {
                ++failures;
            }
        }
    };
    std::thread other(creator);
    creator();
    other.join();
    CHECK(failures.load() == 0);

    const Counters counters = device.counters();
    CHECK(counters.pipelinesCreated == 4 * kRounds);
    // Every render creation hits, and each thread misses the compute pipeline at most once, on its first round, before
    // either thread stored it; at least one compute creation misses.
    CHECK(counters.pipelineCacheHits >= 4 * kRounds - 2);
    CHECK(counters.pipelineCacheHits <= 4 * kRounds - 1);
    destroyDevice(device);
}
