//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12UpscalerTests.cpp
/// @brief Tests the D3D12 FSR upscaler: the runtime's loading, creation, refusals, retirement, and the list state an upscale restores.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/HandlePool.h"
#include "D3D12Addr.h"
#include "D3D12Device.h"
#include "D3D12ListState.h"
#include "D3D12Textures.h"
#include "D3D12Upscaler.h"
#include "ShaderList.h"
#include "Support/Backend.h"
#include "Support/D3D12/D3D12Readback.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <future>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace rojoRHI;

namespace {
    /// How long a case that calls into FidelityFX may run before its watchdog ends the process.
    constexpr std::chrono::seconds kFidelityFxLimit{ 300 };

    //==================================================================================================================
    /// Returns the milliseconds since `start` on the steady clock.
    long long millisecondsSince(std::chrono::steady_clock::time_point start)
    {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    }

    //==================================================================================================================
    /// Runs `body` on the calling thread while a watchdog thread waits at most `limit` for it, and prints how long it took.
    /// When the limit passes first, the watchdog prints `what` and the time elapsed and ends the process with exit code 3, so
    /// a FidelityFX call that never returns fails the run at once instead of holding the step until its timeout.
    template<typename Body> void within(std::chrono::seconds limit, const char* what, Body&& body)
    {
        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        std::promise<void> finished;
        std::future<void> done = finished.get_future();
        std::thread watchdog([&done, limit, what, start] {
            if (done.wait_for(limit) != std::future_status::ready)
            {
                std::fprintf(
                    stderr,
                    "%s did not finish within %lld s (%lld ms elapsed); the watchdog ends the process\n",
                    what,
                    static_cast<long long>(limit.count()),
                    millisecondsSince(start)
                );
                std::fflush(stderr);
                TerminateProcess(GetCurrentProcess(), 3);
            }
        });
        try
        {
            body();
        }
        catch (...)
        {
            finished.set_value();
            watchdog.join();
            throw;
        }
        finished.set_value();
        watchdog.join();
        std::printf("%s finished in %lld ms\n", what, millisecondsSince(start));
        std::fflush(stdout);
    }

    //==================================================================================================================
    /// Returns a new empty directory under the temporary directory, named for this process.
    std::filesystem::path makeEmptyDirectory()
    {
        const std::filesystem::path directory = std::filesystem::temp_directory_path() / std::format("rojorhi-fsr-empty-{}", GetCurrentProcessId());
        std::filesystem::remove_all(directory);
        std::filesystem::create_directories(directory);
        return directory;
    }

    //==================================================================================================================
    /// Returns the references `device` holds, read with an `AddRef` and `Release` pair.
    ULONG references(ID3D12Device12* device)
    {
        device->AddRef();
        return device->Release();
    }

    //==================================================================================================================
    /// Returns the description of a 64x64 to 128x128 upscaler of `kind` with the default formats.
    UpscalerDesc upscalerOf(UpscalerKind kind)
    {
        return UpscalerDesc{ .kind = kind, .input = { 64, 64 }, .output = { 128, 128 }, .label = "test.upscaler" };
    }

    /// True when the build turns GPU-based validation on, under which WARP never completes a FidelityFX upscale dispatch.
#if defined(ROJORHI_D3D12_DEBUG_GBV)
    constexpr bool kGpuBasedValidation = true;
#else
    constexpr bool kGpuBasedValidation = false;
#endif

    /// Why a case that submits an upscale skips under GPU-based validation.
    constexpr const char* kNoUpscaleUnderGbv =
        "WARP never completes a FidelityFX upscale dispatch under GPU-based validation (the floor probe's fsr section, CI run 39463099719)";

    /// The dispatch oracle's threadgroup size; one group writes `kGroupCells` cells.
    constexpr uint32_t kGroupX = 8;
    constexpr uint32_t kGroupY = 4;
    constexpr uint64_t kGroupCells = uint64_t{ kGroupX } * kGroupY * 2;

    /// The dispatch oracle's root.
    struct GridRoot
    {
        Addr output;     ///< Where the entry stores a `uint4` per thread.
        uint32_t width;  ///< The grid's width in threads.
        uint32_t height; ///< The grid's height in threads.
    };

    /// Four `uint32` words of one cell the oracle stores.
    using Cell = std::array<uint32_t, 4>;

    /// The cell an untouched output holds.
    constexpr Cell kUntouched{ 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu };

    /// The width and height of the target the draw renders to.
    constexpr uint32_t kTargetSize = 4;

    /// The target's format, which holds the standard fragment entry's color exactly.
    constexpr std::array<ColorTarget, 1> kTargetFormat = { ColorTarget{ .format = Format::RGBA16Float } };

    /// The half-precision bits of the color the standard fragment entry writes, (0.25, 0.5, 0.75, 1).
    constexpr std::array<uint16_t, 4> kDrawnColor{ 0x3400u, 0x3800u, 0x3A00u, 0x3C00u };

    /// What the pass clears the target to before the draw; each channel differs from the drawn color's.
    constexpr ClearColor kCleared{ 1.0f, 0.0f, 0.0f, 0.0f };

    /// Every subresource of a single-mip, single-layer texture.
    constexpr SubresourceRange kWhole{ .baseMip = 0, .mipCount = 1, .baseLayer = 0, .layerCount = 1 };

    /// Guards `g_messages`, which the message callback may fill from any thread.
    std::mutex g_messagesMutex;

    /// The messages at `Warning` or above the library reported while a case collected them.
    std::vector<std::string> g_messages;

    //==================================================================================================================
    /// Keeps `message` when it is a warning or an error. It may run on any thread.
    void collectMessage(MessageSeverity severity, std::string_view message, void*)
    {
        if (severity == MessageSeverity::Info)
        {
            return;
        }
        const std::lock_guard lock(g_messagesMutex);
        g_messages.emplace_back(message);
    }

    /// Collects the library's warnings and errors, the debug layer's included, into `g_messages`, emptied first, while it
    /// lives, and removes the callback when it goes, however the case ends.
    class MessageCollection
    {
      public:
        MessageCollection();
        ~MessageCollection();

        //==============================================================================================================
        MessageCollection(const MessageCollection&) = delete;

        //==============================================================================================================
        MessageCollection& operator=(const MessageCollection&) = delete;

        /// Returns how many messages were collected, and lists them for the log.
        size_t count() const;
    };

    //==================================================================================================================
    MessageCollection::MessageCollection()
    {
        {
            const std::lock_guard lock(g_messagesMutex);
            g_messages.clear();
        }
        setMessageCallback(collectMessage, nullptr);
    }

    //==================================================================================================================
    MessageCollection::~MessageCollection()
    {
        setMessageCallback(nullptr, nullptr);
    }

    //==================================================================================================================
    size_t MessageCollection::count() const
    {
        const std::lock_guard lock(g_messagesMutex);
        for (const std::string& message : g_messages)
        {
            UNSCOPED_INFO("message: " << message);
        }
        return g_messages.size();
    }

    /// The device, the objects and the recording list of a case that restores list state after vendor work.
    struct Scene
    {
        Device device;    ///< Keeps two frames in flight.
        Pipeline grid;    ///< The dispatch oracle's compute pipeline.
        Pipeline draw;    ///< The standard vertex and fragment pipeline over `kTargetFormat`.
        Buffer cells;     ///< `kGroupCells` cells the oracle writes, each `kUntouched` at creation.
        Texture target;   ///< The `kTargetSize` square target the draw renders to.
        Fence done;       ///< Signalled when the list completes.
        CommandList list; ///< A Graphics list of the open frame.
    };

    //==================================================================================================================
    /// Creates the objects of a scene on a new device, opens a frame and acquires its Graphics list.
    Scene makeScene()
    {
        Scene scene;
        Result<Device> created = createDevice(DeviceDesc{ .framesInFlight = 2 });
        INFO((created ? std::string() : created.error().message));
        REQUIRE(created.has_value());
        scene.device = *created;
        Device& device = scene.device;
        const Result<ShaderLibrary> oracle = device.loadShaderLibrary(test::shaderLibraryFile("DispatchOracle"), "test.DispatchOracle");
        REQUIRE(oracle.has_value());
        const Result<Pipeline> grid = device.createPipeline(ComputePipelineDesc{ .compute = { *oracle, "gridMain" }, .label = "test.gridMain" });
        REQUIRE(grid.has_value());
        scene.grid = *grid;
        const Result<ShaderLibrary> standard = device.loadShaderLibrary(test::standardLibrary(), "test.standard");
        REQUIRE(standard.has_value());
        const Result<Pipeline> draw = device.createPipeline(
            GraphicsPipelineDesc{ .vertex = { *standard, "vsMain" }, .fragment = { *standard, "fsMain" }, .colors = kTargetFormat, .label = "test.standard" }
        );
        INFO((draw ? std::string() : draw.error().message));
        REQUIRE(draw.has_value());
        scene.draw = *draw;
        const std::vector<Cell> untouched(kGroupCells, kUntouched);
        const Result<Buffer> cells = device.createBuffer(
            BufferDesc{ .size = kGroupCells * sizeof(Cell), .usage = BufferUsage::ShaderWrite, .label = "test.cells" }, std::as_bytes(std::span(untouched))
        );
        REQUIRE(cells.has_value());
        scene.cells = *cells;
        const Result<Texture> target = device.createTexture(
            TextureDesc{ .format = Format::RGBA16Float, .extent = { kTargetSize, kTargetSize, 1 }, .usage = TextureUsage::RenderTarget, .label = "test.target" }
        );
        REQUIRE(target.has_value());
        scene.target = *target;
        const Result<Fence> done = device.createFence(0, "test.done");
        REQUIRE(done.has_value());
        scene.done = *done;
        device.beginFrame();
        scene.list = device.acquireCommandList(QueueType::Graphics, "test.list");
        return scene;
    }

    //==================================================================================================================
    /// Returns the native state of `list`.
    d3d12::ListState& stateOf(CommandList list)
    {
        return CommandListImpl::of(list).state;
    }

    //==================================================================================================================
    /// Returns the native calls `list` has recorded since its acquisition.
    uint64_t recordCalls(CommandList list)
    {
        return core::coreOf(list).counts.nativeRecordCalls;
    }

    //==================================================================================================================
    /// Records a barrier on `list` that moves the whole of `texture` from `oldLayout` to `newLayout`, from `srcStage` and
    /// `srcAccess` to `dstStage` and `dstAccess`.
    void transition(CommandList list, Texture texture, Layout oldLayout, Layout newLayout, Stage srcStage, Access srcAccess, Stage dstStage, Access dstAccess)
    {
        const std::array<TextureBarrier, 1> barriers{ TextureBarrier{ .texture = texture,
                                                                      .range = kWhole,
                                                                      .srcStage = srcStage,
                                                                      .srcAccess = srcAccess,
                                                                      .dstStage = dstStage,
                                                                      .dstAccess = dstAccess,
                                                                      .oldLayout = oldLayout,
                                                                      .newLayout = newLayout } };
        list.barrier({}, {}, barriers);
    }

    //==================================================================================================================
    /// Creates a `width` by `height` texture of `format` and `usage` on `device`, and records on `list` its clear to `value`
    /// and the barrier that leaves it in `layout` for compute `access`, as an upscale reads or writes it.
    Texture prepared(
        Device device,
        CommandList list,
        Format format,
        uint32_t width,
        uint32_t height,
        TextureUsage usage,
        const ClearValue& value,
        Layout layout,
        Access access
    )
    {
        const Result<Texture> texture =
            device.createTexture(TextureDesc{ .format = format, .extent = { width, height, 1 }, .usage = usage, .label = "test.upscale" });
        REQUIRE(texture.has_value());
        transition(list, *texture, Layout::Undefined, Layout::CopyDst, Stage::None, Access::None, Stage::Copy, Access::CopyWrite);
        list.clearTexture(*texture, kWhole, value);
        transition(list, *texture, Layout::CopyDst, layout, Stage::Copy, Access::CopyWrite, Stage::ComputeShader, access);
        return *texture;
    }

    //==================================================================================================================
    /// Creates an FSR upscaler of `upscalerOf(Auto)` on `device`, and its inputs and output, and records on `list` the clears
    /// and barriers that leave the inputs in `ShaderRead` and the output in `ShaderWrite`. Returns the upscale's arguments.
    UpscaleParams prepareUpscale(Device device, CommandList list, Upscaler& upscaler)
    {
        const Result<Upscaler> created = device.createUpscaler(upscalerOf(UpscalerKind::Auto));
        REQUIRE(created.has_value());
        upscaler = *created;
        const UpscalerDesc desc = upscalerOf(UpscalerKind::Auto);
        constexpr TextureUsage kInput = TextureUsage::Sampled | TextureUsage::RenderTarget;
        const ClearValue color{ .color = { 0.25f, 0.5f, 0.75f, 1.0f } };
        UpscaleParams params;
        params.color = prepared(device, list, desc.color, desc.input.width, desc.input.height, kInput, color, Layout::ShaderRead, Access::ShaderRead);
        params.depth = prepared(
            device,
            list,
            desc.depth,
            desc.input.width,
            desc.input.height,
            TextureUsage::Sampled | TextureUsage::DepthStencil,
            ClearValue{ .depth = 0.5f },
            Layout::ShaderRead,
            Access::ShaderRead
        );
        params.motion = prepared(device, list, desc.motion, desc.input.width, desc.input.height, kInput, ClearValue{}, Layout::ShaderRead, Access::ShaderRead);
        params.output = prepared(
            device,
            list,
            desc.outputFormat,
            desc.output.width,
            desc.output.height,
            TextureUsage::Storage | TextureUsage::RenderTarget,
            ClearValue{},
            Layout::ShaderWrite,
            Access::ShaderWrite
        );
        params.reset = true;
        params.label = "test.upscale";
        return params;
    }

    //==================================================================================================================
    /// Binds the oracle and its root, one group wide, on the scene's list and dispatches it once, so the compute side holds
    /// the caller's pipeline, the global root signature and the root.
    void bindCompute(Scene& scene)
    {
        scene.list.setPipeline(scene.grid);
        (void)scene.list.pushRoot(GridRoot{ scene.device.address(scene.cells), kGroupX, kGroupY });
        scene.list.dispatch(1, 1, 1);
    }

    /// The native calls the commands after an upscale, or the restore that stands for one, recorded.
    struct RestoreCalls
    {
        uint64_t dispatch = 0;    ///< The dispatch, with no setPipeline before it.
        uint64_t setPipeline = 0; ///< The graphics pipeline's bind in the pass after it.
        uint64_t draw = 0;        ///< The draw in that pass.
    };

    //==================================================================================================================
    /// Records on the scene's list, whose compute side `bindCompute` bound, a dispatch of the oracle without binding it again,
    /// then a pass that clears the target to `kCleared` and draws the standard pipeline over all of it, and the barrier that
    /// readies the target for its copy. Returns the native calls of the dispatch, the bind and the draw.
    RestoreCalls recordAfter(Scene& scene)
    {
        CommandList list = scene.list;
        RestoreCalls calls;
        uint64_t before = recordCalls(list);
        list.dispatch(1, 1, 1);
        calls.dispatch = recordCalls(list) - before;
        transition(
            list, scene.target, Layout::Undefined, Layout::ColorAttachment, Stage::None, Access::None, Stage::ColorAttachment, Access::ColorAttachmentWrite
        );
        const std::array colors{ ColorAttachment{ .texture = scene.target, .load = LoadOp::Clear, .clear = kCleared } };
        list.beginRenderPass(RenderPassDesc{ .colors = colors, .area = { 0, 0, kTargetSize, kTargetSize }, .label = "test.pass" });
        before = recordCalls(list);
        list.setPipeline(scene.draw);
        calls.setPipeline = recordCalls(list) - before;
        list.setViewport(Viewport{ .width = static_cast<float>(kTargetSize), .height = static_cast<float>(kTargetSize) });
        list.setScissor(Rect2D{ 0, 0, kTargetSize, kTargetSize });
        before = recordCalls(list);
        list.draw(3);
        calls.draw = recordCalls(list) - before;
        list.endRenderPass();
        transition(
            list, scene.target, Layout::ColorAttachment, Layout::CopySrc, Stage::ColorAttachment, Access::ColorAttachmentWrite, Stage::Copy, Access::CopyRead
        );
        return calls;
    }

    //==================================================================================================================
    /// Ends the scene's list, submits it on the Graphics queue signalling `done` to 1, ends the frame and waits for the signal.
    void run(Scene& scene)
    {
        scene.list.end();
        const std::array<FenceValue, 1> signal{ FenceValue{ scene.done, 1 } };
        scene.device.queue(QueueType::Graphics).submit(std::span(&scene.list, 1), {}, signal);
        scene.device.endFrame();
        REQUIRE(scene.device.wait({ scene.done, 1 }, UINT64_MAX));
    }

    //==================================================================================================================
    /// Checks what the scene's GPU work left: the oracle's cells of one group and the drawn color in every texel of the target.
    void checkWritten(Scene& scene)
    {
        DeviceImpl& impl = DeviceImpl::of(scene.device);
        const auto& buffer = *static_cast<const d3d12::BufferPayload*>(impl.core.pool<Buffer>().native(scene.cells));
        const std::vector<std::byte> cellBytes = test::readNativeBuffer(impl, buffer.resource, kGroupCells * sizeof(Cell));
        REQUIRE(cellBytes.size() == kGroupCells * sizeof(Cell));
        uint64_t wrongCells = 0;
        for (uint32_t z = 0; z < 2; ++z)
        {
            for (uint32_t y = 0; y < kGroupY; ++y)
            {
                for (uint32_t x = 0; x < kGroupX; ++x)
                {
                    const uint64_t index = (uint64_t{ z } * kGroupY + y) * kGroupX + x;
                    Cell cell{};
                    std::memcpy(cell.data(), cellBytes.data() + index * sizeof(Cell), sizeof(Cell));
                    wrongCells += cell == Cell{ x, y, z, 1 } ? 0 : 1;
                }
            }
        }
        CHECK(wrongCells == 0u);
        const auto& texture = *static_cast<const d3d12::TexturePayload*>(impl.core.pool<Texture>().native(scene.target));
        const std::vector<std::byte> texels = test::readNativeTexture(impl, texture.resource, 0, D3D12_BARRIER_LAYOUT_COPY_SOURCE);
        REQUIRE(texels.size() == size_t{ kTargetSize } * kTargetSize * sizeof(kDrawnColor));
        uint32_t wrongTexels = 0;
        for (size_t texel = 0; texel < size_t{ kTargetSize } * kTargetSize; ++texel)
        {
            std::array<uint16_t, 4> halves{};
            std::memcpy(halves.data(), texels.data() + texel * sizeof(halves), sizeof(halves));
            wrongTexels += halves == kDrawnColor ? 0 : 1;
        }
        CHECK(wrongTexels == 0u);
    }
} // namespace

//======================================================================================================================
TEST_CASE("loadFidelityFx finds nothing in an empty directory", "[d3d12]")
{
    const std::filesystem::path empty = makeEmptyDirectory();
    CHECK_FALSE(d3d12::loadFidelityFx(empty.wstring()).has_value());
    CHECK_FALSE(d3d12::loadFidelityFx(std::wstring_view{}).has_value());
    std::filesystem::remove_all(empty);
}

//======================================================================================================================
TEST_CASE("upscalers lists FSR when the runtime sits beside the executable", "[d3d12]")
{
    const std::filesystem::path beside = std::filesystem::path(d3d12::executableDirectory()) / "amd_fidelityfx_loader_dx12.dll";
    REQUIRE(std::filesystem::exists(beside));
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    REQUIRE(impl.fsr.fidelityFx.has_value());
    // The version query keeps no reference to the device it is given.
    const ULONG before = references(impl.device());
    CHECK(d3d12::fsrAvailable(*impl.fsr.fidelityFx, *impl.device()));
    CHECK(references(impl.device()) == before);
    const std::span<const UpscalerKind> upscalers = created->capabilities().upscalers;
    REQUIRE(upscalers.size() == 1u);
    CHECK(upscalers.front() == UpscalerKind::FSR);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("createUpscaler with Auto creates an FSR context and destroy retires it", "[d3d12]")
{
    // The device's creation queries the runtime and its destruction frees it, so both run under the watchdog too.
    within(kFidelityFxLimit, "createUpscaler with Auto creates an FSR context and destroy retires it", [] {
        Result<Device> created = createDevice(DeviceDesc{ .framesInFlight = 2 });
        REQUIRE(created.has_value());
        Device device = *created;
        DeviceImpl& impl = DeviceImpl::of(device);
        REQUIRE(device.capabilities().upscalers.size() == 1u);
        ID3D12Device12* native = impl.device();
        const ULONG baseline = references(native);
        device.beginFrame();
        const Counters before = device.counters();
        const Result<Upscaler> upscaler = device.createUpscaler(upscalerOf(UpscalerKind::Auto));
        REQUIRE(upscaler.has_value());
        // The pool counts the record, which stands for the context.
        CHECK(device.counters().nativeCreateCalls == before.nativeCreateCalls + 1);
        const core::LookupResult<core::UpscalerRecord> found = impl.core.pool<Upscaler>().lookup(*upscaler);
        REQUIRE(found.record != nullptr);
        CHECK(found.record->kind == UpscalerKind::FSR);
        CHECK(found.record->native != nullptr);
        // The context's objects are children of the device, each holding a reference to it.
        CHECK(references(native) > baseline);
        device.destroy(*upscaler);
        const uint64_t retired = device.counters().deferredDestructionsRetired;
        for (uint32_t frame = 1; frame < impl.core.framesInFlight(); ++frame)
        {
            device.endFrame();
            device.beginFrame();
            CHECK(device.counters().deferredDestructionsRetired == retired);
        }
        device.endFrame();
        const Counters held = device.counters();
        // This frame reuses the slot of the frame the upscaler was destroyed in, so its context is destroyed.
        device.beginFrame();
        const Counters after = device.counters();
        CHECK(after.deferredDestructionsRetired == held.deferredDestructionsRetired + 1);
        CHECK(after.nativeCreateCalls == held.nativeCreateCalls + 1);
        CHECK(impl.core.pool<Upscaler>().liveCount() == 0u);
        CHECK(references(native) == baseline);
        device.endFrame();
        destroyDevice(device);
    });
}

//======================================================================================================================
TEST_CASE("createUpscaler refuses DLSS, XeSS and MetalFX with Unsupported", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    for (const auto& [kind, name] :
         { std::pair{ UpscalerKind::DLSS, "DLSS" }, std::pair{ UpscalerKind::XeSS, "XeSS" }, std::pair{ UpscalerKind::MetalFX, "MetalFX" } })
    {
        const Result<Upscaler> upscaler = created->createUpscaler(upscalerOf(kind));
        REQUIRE_FALSE(upscaler.has_value());
        CHECK(upscaler.error().category == ErrorCategory::Unsupported);
        CHECK(upscaler.error().message.find(name) != std::string::npos);
    }
    CHECK(DeviceImpl::of(*created).core.pool<Upscaler>().liveCount() == 0u);
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("a device whose runtime is absent refuses Auto with Unsupported", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    // The device is made into one built where no runtime sits beside the executable: the seam loads nothing from an empty
    // directory, and the capability report lists what that leaves.
    const std::filesystem::path empty = makeEmptyDirectory();
    std::optional<d3d12::FidelityFx> loaded = std::exchange(impl.fsr.fidelityFx, d3d12::loadFidelityFx(empty.wstring()));
    std::filesystem::remove_all(empty);
    REQUIRE_FALSE(impl.fsr.fidelityFx.has_value());
    Capabilities& capabilities = impl.core.capabilities();
    const std::span<const UpscalerKind> listed = std::exchange(capabilities.upscalers, d3d12::listedUpscalers(impl.fsr.fidelityFx, *impl.device()));
    CHECK(created->capabilities().upscalers.empty());
    for (const UpscalerKind kind : { UpscalerKind::Auto, UpscalerKind::FSR })
    {
        const Result<Upscaler> upscaler = created->createUpscaler(upscalerOf(kind));
        REQUIRE_FALSE(upscaler.has_value());
        CHECK(upscaler.error().category == ErrorCategory::Unsupported);
        CHECK(upscaler.error().message.find("amd_fidelityfx_loader_dx12.dll") != std::string::npos);
    }
    CHECK(impl.core.pool<Upscaler>().liveCount() == 0u);
    impl.fsr.fidelityFx = std::move(loaded);
    capabilities.upscalers = listed;
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("the restore after vendor work sets the heaps at once and the rest at the next dispatch, bind and draw", "[d3d12]")
{
    REQUIRE(kD3D12LibrariesBuilt);
    const MessageCollection messages;
    Scene scene = makeScene();
    bindCompute(scene);
    const d3d12::ListState& state = stateOf(scene.list);
    const d3d12::Binding& binding = state.binding;
    const uint64_t before = recordCalls(scene.list);
    d3d12::resumeAfterVendorWork(CommandListImpl::of(scene.list));
    // SetDescriptorHeaps alone.
    CHECK(recordCalls(scene.list) - before == 1u);
    CHECK(binding.graphicsSignatureDirty);
    CHECK(binding.computeRoot == d3d12::ComputeRoot::Foreign);
    CHECK(binding.graphicsRootDirty);
    CHECK(binding.computeRootDirty);
    CHECK(binding.drawIdDirty);
    CHECK(binding.computePipeline == nullptr);
    CHECK(binding.graphicsPipeline == nullptr);
    CHECK(state.dynamic.topology == D3D_PRIMITIVE_TOPOLOGY_UNDEFINED);
    const RestoreCalls calls = recordAfter(scene);
    // SetComputeRootSignature, SetPipelineState, SetComputeRootShaderResourceView and Dispatch.
    CHECK(calls.dispatch == 4u);
    // SetPipelineState and IASetPrimitiveTopology.
    CHECK(calls.setPipeline == 2u);
    // SetGraphicsRootSignature, SetGraphicsRootShaderResourceView, SetGraphicsRoot32BitConstant and DrawInstanced.
    CHECK(calls.draw == 4u);
    CHECK_FALSE(binding.graphicsSignatureDirty);
    CHECK(binding.computeRoot == d3d12::ComputeRoot::Global);
    run(scene);
    checkWritten(scene);
    CHECK(messages.count() == 0u);
    destroyDevice(scene.device);
}

//======================================================================================================================
TEST_CASE("an upscale sets the heaps again and marks the root signatures, roots, b1, pipeline and topology dirty", "[d3d12]")
{
    if (kGpuBasedValidation)
    {
        SKIP(kNoUpscaleUnderGbv);
    }
    REQUIRE(kD3D12LibrariesBuilt);
    within(kFidelityFxLimit, "an upscale sets the heaps again and marks the root signatures, roots, b1, pipeline and topology dirty", [] {
        const MessageCollection messages;
        Scene scene = makeScene();
        Upscaler upscaler;
        const UpscaleParams params = prepareUpscale(scene.device, scene.list, upscaler);
        bindCompute(scene);
        // A pass that draws leaves the graphics side bound too: its pipeline in the slot, the root, b1 and the topology.
        (void)recordAfter(scene);
        const d3d12::ListState& state = stateOf(scene.list);
        const d3d12::Binding& binding = state.binding;
        REQUIRE_FALSE(binding.graphicsRootDirty);
        REQUIRE_FALSE(binding.drawIdDirty);
        REQUIRE(binding.graphicsPipeline != nullptr);
        REQUIRE(state.dynamic.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        scene.list.upscale(upscaler, params);
        CHECK(binding.graphicsSignatureDirty);
        CHECK(binding.computeRoot == d3d12::ComputeRoot::Foreign);
        CHECK(binding.graphicsRootDirty);
        CHECK(binding.computeRootDirty);
        CHECK(binding.drawIdDirty);
        CHECK(binding.computePipeline == nullptr);
        CHECK(binding.graphicsPipeline == nullptr);
        CHECK(state.dynamic.topology == D3D_PRIMITIVE_TOPOLOGY_UNDEFINED);
        // The upscale's scope is the last the list recorded and holds its label.
        CHECK(core::coreOf(scene.list).scopeLog.back().label == "test.upscale");
        run(scene);
        CHECK(messages.count() == 0u);
        scene.device.destroy(upscaler);
        destroyDevice(scene.device);
    });
}

//======================================================================================================================
TEST_CASE("a dispatch and a draw after an upscale rebind their state and write the expected values", "[d3d12]")
{
    if (kGpuBasedValidation)
    {
        SKIP(kNoUpscaleUnderGbv);
    }
    REQUIRE(kD3D12LibrariesBuilt);
    within(kFidelityFxLimit, "a dispatch and a draw after an upscale rebind their state and write the expected values", [] {
        const MessageCollection messages;
        Scene scene = makeScene();
        Upscaler upscaler;
        const UpscaleParams params = prepareUpscale(scene.device, scene.list, upscaler);
        // The compute side holds the oracle and the root block that names the cells before the upscale, and nothing binds
        // them again after it.
        bindCompute(scene);
        scene.list.upscale(upscaler, params);
        (void)recordAfter(scene);
        run(scene);
        checkWritten(scene);
        CHECK(messages.count() == 0u);
        scene.device.destroy(upscaler);
        destroyDevice(scene.device);
    });
}

//======================================================================================================================
TEST_CASE("an upscale counts one native call and the next dispatch, bind and draw count their restore", "[d3d12]")
{
    if (kGpuBasedValidation)
    {
        SKIP(kNoUpscaleUnderGbv);
    }
    REQUIRE(kD3D12LibrariesBuilt);
    within(kFidelityFxLimit, "an upscale counts one native call and the next dispatch, bind and draw count their restore", [] {
        const MessageCollection messages;
        Scene scene = makeScene();
        Upscaler upscaler;
        const UpscaleParams params = prepareUpscale(scene.device, scene.list, upscaler);
        bindCompute(scene);
        // An empty scope costs what the upscale's own scope costs: a PIX event, and a timestamp pair when the list is timed.
        uint64_t before = recordCalls(scene.list);
        scene.list.beginScope("test.scope");
        scene.list.endScope();
        const uint64_t scope = recordCalls(scene.list) - before;
        before = recordCalls(scene.list);
        scene.list.upscale(upscaler, params);
        // The scope, ffxDispatch and SetDescriptorHeaps.
        CHECK(recordCalls(scene.list) - before == scope + 2);
        const RestoreCalls calls = recordAfter(scene);
        // SetComputeRootSignature, SetPipelineState, SetComputeRootShaderResourceView and Dispatch.
        CHECK(calls.dispatch == 4u);
        // SetPipelineState and IASetPrimitiveTopology.
        CHECK(calls.setPipeline == 2u);
        // SetGraphicsRootSignature, SetGraphicsRootShaderResourceView, SetGraphicsRoot32BitConstant and DrawInstanced.
        CHECK(calls.draw == 4u);
        run(scene);
        CHECK(messages.count() == 0u);
        scene.device.destroy(upscaler);
        destroyDevice(scene.device);
    });
}
