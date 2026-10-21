//----------------------------------------------------------------------------------------------------------------------
/// @file NativeContextChecks.cpp
/// @brief Checks the native arm's D3D12 context on this adapter: device, message callback, environment, arena, shaders.
//----------------------------------------------------------------------------------------------------------------------
#include "Common/CpuRoots.h"
#include "Common/RunOptions.h"
#include "Host/Environment.h"
#include "NativeD3D12/NativeContext.h"

#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

#include <process.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

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

    //==================================================================================================================
    // Opens a context or fails the running case with the reason.
    void openContext(NativeContext& context)
    {
        const std::expected<void, std::string> opened = context.open(runOptions(Mode::Correctness));
        INFO((opened ? std::string() : opened.error()));
        REQUIRE(opened.has_value());
    }
} // namespace

//======================================================================================================================
TEST_CASE("N's device opens on the configured adapter", "[native]")
{
    NativeContext context;
    openContext(context);
    REQUIRE(context.device() != nullptr);
    const DXGI_ADAPTER_DESC1& desc = context.adapterDesc();
#if defined(ROJORHI_D3D12_ADAPTER_WARP)
    ComPtr<IDXGIFactory6> factory;
    REQUIRE(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))));
    ComPtr<IDXGIAdapter1> warp;
    REQUIRE(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))));
    DXGI_ADAPTER_DESC1 warpDesc{};
    REQUIRE(SUCCEEDED(warp->GetDesc1(&warpDesc)));
    CHECK(desc.AdapterLuid.LowPart == warpDesc.AdapterLuid.LowPart);
    CHECK(desc.AdapterLuid.HighPart == warpDesc.AdapterLuid.HighPart);
    CHECK((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0u);
#else
    CHECK((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0u);
#endif
}

//======================================================================================================================
TEST_CASE("N's device registers its message callback under the debug layer", "[native]")
{
    if constexpr (NativeContext::kDebugLayer)
    {
        NativeContext context;
        openContext(context);
        CHECK(context.messageCookie() != 0u);
    }
    else
    {
        SKIP("the build enables no D3D12 debug layer (rojorhi_d3d12_debug is off, or mode in Release), so N registers no callback");
    }
}

//======================================================================================================================
TEST_CASE("N and RojoRHI create their devices on the same adapter", "[native]")
{
    NativeContext context;
    openContext(context);
    rojoRHI::DeviceDesc desc{};
    desc.label = "bench.adapter";
    rojoRHI::Result<rojoRHI::Device> device = rojoRHI::createDevice(desc);
    INFO((device ? std::string() : device.error().message));
    REQUIRE(device.has_value());
    const std::string rojoAdapter(device->capabilities().adapterName);
    rojoRHI::destroyDevice(*device);
    CHECK(rojoAdapter == context.adapterName());
}

//======================================================================================================================
TEST_CASE("the environment command writes every D3D12 field", "[native]")
{
    const std::filesystem::path path = std::filesystem::temp_directory_path() / ("rojorhi-bench-environment-" + std::to_string(_getpid()) + ".json");
    const std::expected<void, std::string> written = writeEnvironmentFile(path);
    INFO((written ? std::string() : written.error()));
    REQUIRE(written.has_value());
    std::ifstream file(path, std::ios::binary);
    const std::string text{ std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>() };
    file.close();
    std::filesystem::remove(path);
    INFO(text);
    const std::set<std::string> expected{ "device",  "adapter", "vendorId",    "deviceId", "driverVersion", "featureLevel", "shaderModel",
                                          "agility", "warp",    "timestampHz", "os",       "thermal" };
    CHECK(topLevelKeys(text) == expected);
    CHECK(text.find("\"device\":null") == std::string::npos);
    CHECK(text.find("\"timestampHz\":null") == std::string::npos);
    CHECK(text.find("\"agility\":null") == std::string::npos);
    CHECK(text.find("\"thermal\":\"unknown\"") != std::string::npos);
    CHECK(text.find("\"os\":\"Windows ") != std::string::npos);
}

//======================================================================================================================
TEST_CASE("N's upload arena refuses an overrun with an error", "[native]")
{
    NativeContext context;
    openContext(context);
    REQUIRE(context.prepareFrames(NativeFrameShape{ .lists = 1, .arenaBytes = NativeContext::kArenaAlignment }).has_value());
    const std::array<std::byte, 200> block{};

    NativeContext::Slot& slot = context.beginFrame(0, 0);
    CHECK(slot.push(block) == slot.arenaAddress);
    // The second block starts at the next 256-byte boundary, past the arena's end: nothing is written.
    CHECK(slot.push(block) == slot.arenaAddress);
    CHECK(slot.arenaUsed == block.size());
    const std::expected<std::optional<double>, std::string> overrun = context.endFrame();
    REQUIRE(!overrun.has_value());
    CHECK(overrun.error().find("arena") != std::string::npos);
    CHECK(overrun.error().find("200 bytes at offset 256") != std::string::npos);

    // A later frame of the same slot starts with an empty arena.
    NativeContext::Slot& again = context.beginFrame(3, 0);
    CHECK(&again == &slot);
    CHECK(again.push(block) == again.arenaAddress);
    const std::expected<std::optional<double>, std::string> fits = context.endFrame();
    INFO((fits ? std::string() : fits.error()));
    CHECK(fits.has_value());
}

//======================================================================================================================
TEST_CASE("a native shader library that was not built is reported with its path", "[native]")
{
    NativeContext context;
    openContext(context);
    const std::expected<D3D12_SHADER_BYTECODE, std::string> library = context.library("BenchMissing");
    REQUIRE(!library.has_value());
    CHECK(library.error().find("Shaders") != std::string::npos);
    CHECK(library.error().find("BenchMissing.dxil") != std::string::npos);
}

//======================================================================================================================
TEST_CASE("N's global root signature runs a module compute shader that stores through an Addr", "[native]")
{
    NativeContext context;
    openContext(context);
    const std::expected<D3D12_SHADER_BYTECODE, std::string> shader = context.library("BenchCompute", "storeMain");
    INFO((shader ? std::string() : shader.error()));
    REQUIRE(shader.has_value());
    const std::expected<ID3D12PipelineState*, std::string> pipeline = context.computePipeline(*shader, "bench.check.store");
    INFO((pipeline ? std::string() : pipeline.error()));
    REQUIRE(pipeline.has_value());
    const std::expected<NativeBuffer, std::string> target = context.defaultBuffer(16, "bench.check.target");
    REQUIRE(target.has_value());
    const std::expected<uint32_t, std::string> views = context.bufferViews(target->resource);
    REQUIRE(views.has_value());
    CHECK(*views >= NativeContext::kFirstFreeView);

    StoreRoot root;
    root.address = rojoRHI::Addr{ *views, 4 };
    root.value = 0x1234'5678u;
    const std::expected<NativeBuffer, std::string> rootBuffer = context.uploadBuffer(sizeof(root), "bench.check.root", std::as_bytes(std::span(&root, 1)));
    REQUIRE(rootBuffer.has_value());
    const std::expected<void, std::string> ran = context.runSetup([&](ID3D12GraphicsCommandList9& list) {
        list.SetDescriptorHeaps(static_cast<UINT>(context.heaps().size()), context.heaps().data());
        list.SetComputeRootSignature(context.rootSignature());
        list.SetPipelineState(*pipeline);
        list.SetComputeRootShaderResourceView(0, rootBuffer->address);
        list.Dispatch(1, 1, 1);
    });
    INFO((ran ? std::string() : ran.error()));
    REQUIRE(ran.has_value());

    const std::expected<std::vector<std::byte>, std::string> bytes = context.readBuffer(target->resource, 0, 16);
    INFO((bytes ? std::string() : bytes.error()));
    REQUIRE(bytes.has_value());
    REQUIRE(bytes->size() == 16);
    std::array<uint32_t, 4> words{};
    std::memcpy(words.data(), bytes->data(), 16);
    CHECK(words == std::array<uint32_t, 4>{ 0, 0x1234'5678u, 0, 0 });
}

//======================================================================================================================
TEST_CASE("the native arm signals its frame fence at each frame's end and waits on it before reusing a frame slot", "[native]")
{
    // A slot's first frame waits on fence value 0, which the context skips without a call (D3D12 warns on a zero-value
    // wait), so it makes one host call fewer than a reused slot's frame, which arms its event and waits. Frame index
    // signals index + 1.
    NativeContext context;
    openContext(context);
    REQUIRE(context.prepareFrames(NativeFrameShape{}).has_value());
    REQUIRE(context.frameFence() != nullptr);
    constexpr uint32_t kFrames = 2 * NativeContext::kFrameSlots;
    for (uint32_t index = 0; index < kFrames; ++index)
    {
        CAPTURE(index);
        const NativeCalls before = context.calls();
        (void)context.beginFrame(index, 0);
        CHECK(context.calls().host == before.host + (index < NativeContext::kFrameSlots ? 1u : 2u));
        REQUIRE(context.endFrame().has_value());
        CHECK(context.calls().submit == before.submit + 1);
        CHECK(context.calls().record == before.record);
    }
    // A setup submission waits for every frame signalled before it, so the fence then holds the last frame's value.
    REQUIRE(context.runSetup([](ID3D12GraphicsCommandList9&) {}).has_value());
    CHECK(context.frameFence()->GetCompletedValue() == kFrames);
}
