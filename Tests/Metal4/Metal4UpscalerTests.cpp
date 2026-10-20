//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4UpscalerTests.cpp
/// @brief Tests the Metal 4 MetalFX upscaler: capability, creation, an upscale's native calls and fence, and retirement.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/Counters.h"
#include "Core/DeviceCore.h"
#include "Metal4Barriers.h"
#include "Metal4Common.h"
#include "Metal4Device.h"
#include "Metal4Encoders.h"
#include "Metal4Upscaler.h"
#include "Support/Metal4/Metal4Timing.h"
#include "Support/UpscaleKit.h"

#include <catch2/catch_test_macros.hpp>

#include <Metal/Metal.hpp>
#include <MetalFX/MetalFX.hpp>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

using namespace rojoRHI;
using rojoRHI::test::UpscaleKit;

namespace {
    /// The native calls an upscale sends with no encoder open and no barrier pending: `pushDebugGroup` and the begin
    /// timestamp; the carrier's `computeCommandEncoder`, `setLabel`, its queue barrier after every stage, `updateFence` and
    /// `endEncoding`; the scaler's six textures, content width and height, two jitter offsets, two motion scales,
    /// pre-exposure and reset; `encodeToCommandBuffer`; the end timestamp and `popDebugGroup`.
    constexpr uint64_t kUpscaleCalls = 2 + 5 + 14 + 1 + 2;

    //==================================================================================================================
    /// What `list` has counted so far; fails the case when a scope it recorded holds no timestamp pair, since the pinned
    /// counts include both timestamp writes.
    const core::RecordCounts& countsOf(CommandList list)
    {
        return test::timedCounts(list);
    }

    //==================================================================================================================
    /// The barrier state of `list`.
    const metal4::BarrierState& barriersOf(CommandList list)
    {
        return CommandListImpl::of(list).encoders.barriers();
    }

    //==================================================================================================================
    /// The backend state of `upscaler`.
    metal4::UpscalerState& stateOf(Device device, Upscaler upscaler)
    {
        return *static_cast<metal4::UpscalerState*>(DeviceImpl::of(device).core.pool<Upscaler>().native(upscaler));
    }

    //==================================================================================================================
    /// The native texture `texture` wraps.
    MTL::Texture* nativeOf(Device device, Texture texture)
    {
        return static_cast<MTL::Texture*>(DeviceImpl::of(device).core.pool<Texture>().native(texture));
    }

    //==================================================================================================================
    /// Ends the kit's list, submits it, waits for it on the host and ends the frame.
    void submitAndWait(UpscaleKit& kit)
    {
        Device device = kit.kit.device;
        kit.kit.list.end();
        const Fence fence = *device.createFence(0, "test.done");
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
        device.queue(QueueType::Graphics).submit(std::array{ kit.kit.list }, {}, signal);
        CHECK(device.wait({ fence, 1 }, UINT64_MAX));
        device.endFrame();
    }

    //==================================================================================================================
    /// Runs `count` empty frames.
    void runFrames(Device device, uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            device.beginFrame();
            device.endFrame();
        }
    }

    //==================================================================================================================
    /// A global barrier from copy writes to copy reads and writes.
    GlobalBarrier copyToCopy()
    {
        return GlobalBarrier{
            .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead | Access::CopyWrite
        };
    }
} // namespace

//======================================================================================================================
TEST_CASE("the device lists MetalFX when MetalFX supports it", "[metal4]")
{
    Device device = *createDevice();
    const std::span<const UpscalerKind> upscalers = device.capabilities().upscalers;
    if (MTLFX::TemporalScalerDescriptor::supportsMetal4FX(DeviceImpl::of(device).device))
    {
        REQUIRE(upscalers.size() == 1);
        CHECK(upscalers.front() == UpscalerKind::MetalFX);
    }
    else
    {
        CHECK(upscalers.empty());
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("Auto creates a MetalFX upscaler", "[metal4]")
{
    Device device = *createDevice();
    if (device.capabilities().upscalers.empty())
    {
        SKIP("MetalFX does not support this device");
    }
    const Counters before = device.counters();
    const Result<Upscaler> upscaler = device.createUpscaler(UpscalerDesc{ .input = { 64, 64 }, .output = { 128, 128 }, .label = "test.upscaler" });
    REQUIRE(upscaler.has_value());
    const metal4::UpscalerState& state = stateOf(device, *upscaler);
    REQUIRE(state.scaler != nullptr);
    REQUIRE(state.fence != nullptr);
    REQUIRE(state.mask != nullptr);
    CHECK(state.scaler->inputWidth() == 64);
    CHECK(state.scaler->outputWidth() == 128);
    CHECK(state.scaler->colorTextureFormat() == MTL::PixelFormatRGBA16Float);
    CHECK(state.scaler->depthTextureFormat() == MTL::PixelFormatDepth32Float);
    CHECK(state.scaler->motionTextureFormat() == MTL::PixelFormatRG16Float);
    CHECK(state.scaler->outputTextureFormat() == MTL::PixelFormatRGBA16Float);
    CHECK(state.scaler->fence() == state.fence);
    CHECK_FALSE(state.scaler->isDepthReversed());
    // The scale range starts at the whole input's scale and reaches what the device supports.
    CHECK(state.scaler->inputContentMinScale() == 2.0f);
    CHECK(state.scaler->inputContentMaxScale() == MTLFX::TemporalScalerDescriptor::supportedInputContentMaxScale(DeviceImpl::of(device).device));
    {
        // The getter returns an autoreleased string.
        const metal4::AutoreleasePool pool;
        CHECK(metal4::toStdString(state.fence->label()) == "test.upscaler.fence");
    }
    CHECK(state.mask->pixelFormat() == MTL::PixelFormatR8Unorm);
    CHECK(state.mask->width() == 64);
    CHECK(state.mask->height() == 64);
    CHECK(state.mask->storageMode() == MTL::StorageModePrivate);
    CHECK(state.mask->usage() == MTL::TextureUsageShaderRead);
    CHECK(DeviceImpl::of(device).residency.contains(state.mask));
    // The pool's record, then the fence and the mask the backend makes for it, and the mask's staging buffer.
    CHECK(device.counters().nativeCreateCalls == before.nativeCreateCalls + 4);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("ReversedDepth sets the scaler's depthReversed", "[metal4]")
{
    Device device = *createDevice();
    if (device.capabilities().upscalers.empty())
    {
        SKIP("MetalFX does not support this device");
    }
    const Upscaler upscaler = *device.createUpscaler(UpscalerDesc{ .input = { 32, 16 }, .output = { 32, 16 }, .flags = UpscalerFlags::ReversedDepth });
    CHECK(stateOf(device, upscaler).scaler->isDepthReversed());
    CHECK(stateOf(device, upscaler).scaler->inputContentMinScale() == 1.0f);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("createUpscaler runs on several threads at once", "[metal4]")
{
    // MetalFX fills a process-wide table of Metal's at a scaler's creation without a lock of its own; two creations at once
    // corrupt it, so the backend creates scalers one at a time. A first creation in a fresh process races widest.
    Device device = *createDevice();
    if (device.capabilities().upscalers.empty())
    {
        SKIP("MetalFX does not support this device");
    }
    std::atomic<int> failures = 0;
    std::vector<std::thread> creators;
    for (int t = 0; t < 4; ++t)
    {
        creators.emplace_back([&device, &failures, t] {
            for (int i = 0; i < 3; ++i)
            {
                const UpscalerFlags flags = (i + t) % 2 == 0 ? UpscalerFlags::None : UpscalerFlags::ReversedDepth;
                const Result<Upscaler> upscaler = device.createUpscaler(UpscalerDesc{ .input = { 64, 64 }, .output = { 128, 128 }, .flags = flags });
                if (!upscaler)
                {
                    ++failures;
                    continue;
                }
                device.destroy(*upscaler);
            }
        });
    }
    for (std::thread& creator : creators)
    {
        creator.join();
    }
    CHECK(failures.load() == 0);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an extent ratio beyond the device's scale range is Unsupported and allocates nothing", "[metal4]")
{
    Device device = *createDevice();
    if (device.capabilities().upscalers.empty())
    {
        SKIP("MetalFX does not support this device");
    }
    const float maxScale = MTLFX::TemporalScalerDescriptor::supportedInputContentMaxScale(DeviceImpl::of(device).device);
    const auto output = static_cast<uint32_t>(16.0f * maxScale) + 1;
    const uint64_t live = DeviceImpl::of(device).core.pool<Upscaler>().liveCount();
    const Result<Upscaler> upscaler = device.createUpscaler(UpscalerDesc{ .input = { 16, 16 }, .output = { output, 16 } });
    REQUIRE_FALSE(upscaler.has_value());
    CHECK(upscaler.error().category == ErrorCategory::Unsupported);
    CHECK(upscaler.error().message.find("MetalFX") != std::string::npos);
    CHECK(DeviceImpl::of(device).core.pool<Upscaler>().liveCount() == live);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a kind Metal cannot create is Unsupported and allocates nothing", "[metal4]")
{
    Device device = *createDevice();
    const uint64_t live = DeviceImpl::of(device).core.pool<Upscaler>().liveCount();
    for (const UpscalerKind kind : { UpscalerKind::DLSS, UpscalerKind::FSR, UpscalerKind::XeSS })
    {
        const Result<Upscaler> upscaler = device.createUpscaler(UpscalerDesc{ .kind = kind, .input = { 64, 64 }, .output = { 128, 128 } });
        REQUIRE_FALSE(upscaler.has_value());
        CHECK(upscaler.error().category == ErrorCategory::Unsupported);
    }
    CHECK(DeviceImpl::of(device).core.pool<Upscaler>().liveCount() == live);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an upscale sends its debug group, a carrier encoder with the fence update, the scaler's setters and one encode", "[metal4]")
{
    UpscaleKit kit;
    const core::RecordCounts before = countsOf(kit.kit.list);
    kit.upscale();
    CHECK(countsOf(kit.kit.list).nativeRecordCalls == before.nativeRecordCalls + kUpscaleCalls);
    CHECK(countsOf(kit.kit.list).encodersOpened == before.encodersOpened + 1);
    // The carrier's barrier waits for every earlier stage on the queue, an earlier frame's MetalFX work included.
    CHECK(countsOf(kit.kit.list).nativeBarriers == before.nativeBarriers + 1);
    CHECK(barriersOf(kit.kit.list).owedFence == stateOf(kit.kit.device, kit.upscaler).fence);
    CHECK(barriersOf(kit.kit.list).owedFenceStages == (metal4::kComputeStages | metal4::kRenderStages));
    CHECK(CommandListImpl::of(kit.kit.list).encoders.kind() == metal4::EncoderKind::None);
    CHECK(CommandListImpl::of(kit.kit.list).state.scopeLabels.empty());
    // An open encoder ends first.
    const Buffer a = *kit.kit.device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *kit.kit.device.createBuffer({ .size = 256, .label = "test.b" });
    const Upscaler other = *kit.kit.device.createUpscaler(kit.desc);
    kit.kit.list.copyBuffer(a, 0, b, 0, 256);
    const core::RecordCounts open = countsOf(kit.kit.list);
    kit.kit.list.upscale(other, kit.params);
    // The copy's encoder ends; it took the compute wait on the first upscale's fence, and the carrier takes the render wait
    // still owed, so the second fence, which replaces the first, follows the first upscale's work.
    CHECK(countsOf(kit.kit.list).nativeRecordCalls == open.nativeRecordCalls + 1 + kUpscaleCalls + 1);
    CHECK(barriersOf(kit.kit.list).owedFence == stateOf(kit.kit.device, other).fence);
    CHECK(barriersOf(kit.kit.list).owedFenceStages == (metal4::kComputeStages | metal4::kRenderStages));
    submitAndWait(kit);
    destroyDevice(kit.kit.device);
}

//======================================================================================================================
TEST_CASE("the encoder after an upscale waits on its fence, once", "[metal4]")
{
    SECTION("a copy")
    {
        UpscaleKit kit;
        const Buffer a = *kit.kit.device.createBuffer({ .size = 256, .label = "test.a" });
        const Buffer b = *kit.kit.device.createBuffer({ .size = 256, .label = "test.b" });
        kit.upscale();
        const core::RecordCounts before = countsOf(kit.kit.list);
        kit.kit.list.copyBuffer(a, 0, b, 0, 256);
        // computeCommandEncoder, setLabel, setArgumentTable, waitForFence, then the copy.
        CHECK(countsOf(kit.kit.list).nativeRecordCalls == before.nativeRecordCalls + 5);
        // The first render encoder still owes its wait.
        CHECK(barriersOf(kit.kit.list).owedFenceStages == metal4::kRenderStages);
        // A later compute encoder owes nothing: the barrier ends the encoder, and the next opens with the barrier and no wait.
        const core::RecordCounts second = countsOf(kit.kit.list);
        kit.kit.list.barrier(std::array{ copyToCopy() });
        kit.kit.list.copyBuffer(b, 0, a, 0, 256);
        CHECK(countsOf(kit.kit.list).nativeRecordCalls == second.nativeRecordCalls + 1 + 3 + 1 + 1);
        submitAndWait(kit);
        destroyDevice(kit.kit.device);
    }
    SECTION("a kernel fill")
    {
        // The same fill on a list with no upscale before it is the baseline.
        UpscaleKit kit;
        const Buffer target = *kit.kit.device.createBuffer({ .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.target" });
        const core::RecordCounts plain = countsOf(kit.kit.list);
        kit.kit.list.fillBuffer(target, 0, 256, 0x01020304u);
        const uint64_t baseline = countsOf(kit.kit.list).nativeRecordCalls - plain.nativeRecordCalls;
        kit.kit.list.beginScope("test.scope");
        kit.kit.list.endScope();
        kit.upscale();
        const core::RecordCounts before = countsOf(kit.kit.list);
        kit.kit.list.fillBuffer(target, 0, 256, 0x01020304u);
        CHECK(countsOf(kit.kit.list).nativeRecordCalls == before.nativeRecordCalls + baseline + 1);
        CHECK(barriersOf(kit.kit.list).owedFenceStages == metal4::kRenderStages);
        submitAndWait(kit);
        destroyDevice(kit.kit.device);
    }
    SECTION("a list that ends right after")
    {
        UpscaleKit kit;
        kit.upscale();
        const core::RecordCounts before = countsOf(kit.kit.list);
        kit.kit.list.end();
        // No barrier is pending, but the fence is: the trailing encoder waits on it and holds back every stage of every later
        // encoder on the queue, so a later list's reads of the output follow MetalFX's writes. computeCommandEncoder,
        // setLabel, waitForFence, barrierAfterStages and endEncoding, then endCommandBuffer.
        CHECK(countsOf(kit.kit.list).nativeRecordCalls == before.nativeRecordCalls + 5 + 1);
        CHECK(countsOf(kit.kit.list).encodersOpened == before.encodersOpened + 1);
        CHECK(countsOf(kit.kit.list).nativeBarriers == before.nativeBarriers + 1);
        CHECK(barriersOf(kit.kit.list).owedFence == nullptr);
        const std::array<CommandList, 1> lists{ kit.kit.list };
        kit.kit.device.queue(QueueType::Graphics).submit(lists);
        kit.kit.device.endFrame();
        destroyDevice(kit.kit.device);
    }
    SECTION("a list whose encoders took both waits")
    {
        UpscaleKit kit;
        const Buffer a = *kit.kit.device.createBuffer({ .size = 256, .label = "test.a" });
        const Buffer b = *kit.kit.device.createBuffer({ .size = 256, .label = "test.b" });
        const Texture c = kit.make(Format::RGBA8Unorm, kit.desc.input, TextureUsage::RenderTarget);
        kit.upscale();
        kit.kit.list.copyBuffer(a, 0, b, 0, 256);
        kit.kit.list.clearTexture(c, {}, ClearValue{});
        REQUIRE(barriersOf(kit.kit.list).owedFenceStages == 0);
        // The fence stays the list's until it ends: the trailing encoder still waits on it, because later lists see only it.
        CHECK(barriersOf(kit.kit.list).owedFence == stateOf(kit.kit.device, kit.upscaler).fence);
        const core::RecordCounts before = countsOf(kit.kit.list);
        kit.kit.list.end();
        CHECK(countsOf(kit.kit.list).nativeRecordCalls == before.nativeRecordCalls + 5 + 1);
        CHECK(countsOf(kit.kit.list).nativeBarriers == before.nativeBarriers + 1);
        const std::array<CommandList, 1> lists{ kit.kit.list };
        kit.kit.device.queue(QueueType::Graphics).submit(lists);
        kit.kit.device.endFrame();
        destroyDevice(kit.kit.device);
    }
    SECTION("a list that ends with a barrier pending")
    {
        UpscaleKit kit;
        kit.upscale();
        const GlobalBarrier after{ .srcStage = Stage::ComputeShader, .srcAccess = Access::ShaderWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead };
        kit.kit.list.barrier(std::array{ after });
        const core::RecordCounts before = countsOf(kit.kit.list);
        kit.kit.list.end();
        // The trailing encoder waits on the fence before its two barriers, so the work it holds back follows MetalFX's: its
        // five calls, waitForFence and endCommandBuffer.
        CHECK(countsOf(kit.kit.list).nativeRecordCalls == before.nativeRecordCalls + 5 + 1 + 1);
        CHECK(countsOf(kit.kit.list).nativeBarriers == before.nativeBarriers + 2);
        const std::array<CommandList, 1> lists{ kit.kit.list };
        kit.kit.device.queue(QueueType::Graphics).submit(lists);
        kit.kit.device.endFrame();
        destroyDevice(kit.kit.device);
    }
}

//======================================================================================================================
TEST_CASE("the first compute and the first render encoder after an upscale each wait on its fence", "[metal4]")
{
    UpscaleKit kit;
    const Buffer b = *kit.kit.device.createBuffer({ .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.b" });
    const Buffer d = *kit.kit.device.createBuffer({ .size = 256, .label = "test.d" });
    const Texture c = kit.make(Format::RGBA8Unorm, kit.desc.input, TextureUsage::RenderTarget);
    kit.kit.list.fillBuffer(b, 0, 256, 0x01020304u);
    kit.kit.list.barrier(std::array{ copyToCopy() });
    kit.upscale();
    // The carrier took the barrier's dispatch and blit part; the fragment part stays pending.
    REQUIRE(barriersOf(kit.kit.list).pendingDestination == MTL::StageFragment);
    const core::RecordCounts beforeClear = countsOf(kit.kit.list);
    kit.kit.list.clearTexture(c, {}, ClearValue{});
    // renderCommandEncoder, setLabel, waitForFence for every render stage, the fragment barrier and endEncoding.
    CHECK(countsOf(kit.kit.list).nativeRecordCalls == beforeClear.nativeRecordCalls + 5);
    CHECK(countsOf(kit.kit.list).nativeBarriers == beforeClear.nativeBarriers + 1);
    CHECK(barriersOf(kit.kit.list).owedFenceStages == metal4::kComputeStages);
    CHECK(barriersOf(kit.kit.list).owedFence == stateOf(kit.kit.device, kit.upscaler).fence);
    // The copy's compute encoder finds no barrier pending, so only the fence orders it after the fill, through MetalFX's
    // work and the carrier: computeCommandEncoder, setLabel, setArgumentTable, waitForFence and the copy.
    const core::RecordCounts beforeCopy = countsOf(kit.kit.list);
    kit.kit.list.copyBuffer(b, 0, d, 0, 256);
    CHECK(countsOf(kit.kit.list).nativeRecordCalls == beforeCopy.nativeRecordCalls + 5);
    CHECK(countsOf(kit.kit.list).nativeBarriers == beforeCopy.nativeBarriers);
    CHECK(barriersOf(kit.kit.list).owedFenceStages == 0);
    CHECK(barriersOf(kit.kit.list).owedFence == stateOf(kit.kit.device, kit.upscaler).fence);
    submitAndWait(kit);
    destroyDevice(kit.kit.device);
}

//======================================================================================================================
TEST_CASE("a render pass after an upscale waits on its fence on the render encoder, once", "[metal4]")
{
    UpscaleKit kit;
    kit.upscale();
    // The same pass twice: the first render encoder after the upscale waits on its fence for every render stage, the second
    // owes nothing.
    const auto pass = [&] {
        const core::RecordCounts before = countsOf(kit.kit.list);
        kit.kit.readyToDraw();
        kit.kit.list.draw(3);
        kit.kit.list.endRenderPass();
        return countsOf(kit.kit.list).nativeRecordCalls - before.nativeRecordCalls;
    };
    const uint64_t first = pass();
    CHECK(barriersOf(kit.kit.list).owedFenceStages == metal4::kComputeStages);
    CHECK(barriersOf(kit.kit.list).owedFence == stateOf(kit.kit.device, kit.upscaler).fence);
    const uint64_t second = pass();
    // The first pass's wait on the fence; the second pass's beginRenderPass ends the first pass's encoder, which stayed open.
    CHECK(first == second);
    CHECK(barriersOf(kit.kit.list).owedFenceStages == metal4::kComputeStages);
    submitAndWait(kit);
    destroyDevice(kit.kit.device);
}

//======================================================================================================================
TEST_CASE(
    "a barrier pending before an upscale is covered by the carrier's barrier for its stages and stays pending for later encoders' other stages", "[metal4]"
)
{
    UpscaleKit kit;
    const GlobalBarrier before{ .srcStage = Stage::ColorAttachment | Stage::Copy,
                                .srcAccess = Access::ColorAttachmentWrite | Access::CopyWrite,
                                .dstStage = Stage::ComputeShader | Stage::FragmentShader,
                                .dstAccess = Access::ShaderRead };
    kit.kit.list.barrier(std::array{ before });
    REQUIRE(barriersOf(kit.kit.list).pendingDestination == (MTL::StageDispatch | MTL::StageFragment));
    const core::RecordCounts start = countsOf(kit.kit.list);
    kit.upscale();
    // The carrier's one barrierAfterQueueStages, after every stage, covers the pending part on its dispatch and blit stages.
    CHECK(countsOf(kit.kit.list).nativeRecordCalls == start.nativeRecordCalls + kUpscaleCalls);
    CHECK(countsOf(kit.kit.list).nativeBarriers == start.nativeBarriers + 1);
    CHECK(barriersOf(kit.kit.list).pendingDestination == MTL::StageFragment);
    CHECK(barriersOf(kit.kit.list).pendingSource == (MTL::StageFragment | MTL::StageBlit | MTL::StageDispatch));
    CHECK(barriersOf(kit.kit.list).openDestination == 0);
    submitAndWait(kit);
    destroyDevice(kit.kit.device);
}

//======================================================================================================================
TEST_CASE("a null reactive binds the cleared mask, a given one binds itself", "[metal4]")
{
    UpscaleKit kit;
    const metal4::UpscalerState& state = stateOf(kit.kit.device, kit.upscaler);
    kit.upscale();
    CHECK(state.scaler->reactiveMaskTexture() == state.mask);
    CHECK(state.scaler->exposureTexture() == nullptr);
    CHECK(state.scaler->colorTexture() == nativeOf(kit.kit.device, kit.params.color));
    CHECK(state.scaler->outputTexture() == nativeOf(kit.kit.device, kit.params.output));
    // A whole-input content rectangle when inputContent is zero.
    CHECK(state.scaler->inputContentWidth() == UpscaleKit::kInput);
    CHECK(state.scaler->inputContentHeight() == UpscaleKit::kInput);
    kit.nextFrame();
    kit.params.reactive = kit.reactiveTexture();
    kit.params.exposure = kit.exposureTexture();
    kit.params.inputContent = { 48, 44 };
    kit.params.jitterX = 0.25f;
    kit.params.motionScaleY = -2.0f;
    kit.params.preExposure = 2.0f;
    kit.params.reset = true;
    kit.upscale();
    CHECK(state.scaler->reactiveMaskTexture() == nativeOf(kit.kit.device, kit.params.reactive));
    CHECK(state.scaler->exposureTexture() == nativeOf(kit.kit.device, kit.params.exposure));
    CHECK(state.scaler->inputContentWidth() == 48);
    CHECK(state.scaler->inputContentHeight() == 44);
    CHECK(state.scaler->jitterOffsetX() == 0.25f);
    CHECK(state.scaler->motionVectorScaleY() == -2.0f);
    CHECK(state.scaler->preExposure() == 2.0f);
    submitAndWait(kit);
    destroyDevice(kit.kit.device);
}

//======================================================================================================================
TEST_CASE("a destroyed upscaler releases its objects when its frame slot retires", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    if (device.capabilities().upscalers.empty())
    {
        destroyDevice(device);
        SKIP("MetalFX does not support this device");
    }
    DeviceImpl& impl = DeviceImpl::of(device);
    const Upscaler upscaler = *device.createUpscaler(UpscalerDesc{ .input = { 64, 64 }, .output = { 128, 128 } });
    const metal4::UpscalerState& state = stateOf(device, upscaler);
    // Kept alive past retirement, so the set and the counts can still be asked about them.
    MTL::Texture* mask = state.mask;
    MTL::Fence* fence = state.fence;
    MTL4FX::TemporalScaler* scaler = state.scaler;
    mask->retain();
    fence->retain();
    scaler->retain();
    CHECK(impl.residency.contains(mask));
    device.destroy(upscaler);
    runFrames(device, 2);
    CHECK(impl.residency.contains(mask));
    const Counters before = device.counters();
    runFrames(device, 1);
    CHECK_FALSE(impl.residency.contains(mask));
    CHECK(scaler->retainCount() == 1);
    // The pool counts the record's release; the fence and the mask count one each.
    CHECK(device.counters().nativeCreateCalls == before.nativeCreateCalls + 3);
    {
        const metal4::AutoreleasePool pool;
        scaler->release();
        fence->release();
        mask->release();
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a content extent below the scaler's range is raised to the smallest it takes", "[metal4]")
{
    UpscaleKit kit;
    const metal4::UpscalerState& state = stateOf(kit.kit.device, kit.upscaler);
    const float maxScale = state.scaler->inputContentMaxScale();
    // The smallest content whose scale to the output stays within the scaler's maximum, in each axis.
    const auto smallest = static_cast<uint32_t>(std::ceil(static_cast<float>(UpscaleKit::kOutput) / maxScale));
    REQUIRE(smallest > 1);
    REQUIRE(smallest <= UpscaleKit::kInput);
    kit.params.inputContent = { 1, UpscaleKit::kInput };
    kit.upscale();
    CHECK(state.scaler->inputContentWidth() == smallest);
    CHECK(state.scaler->inputContentHeight() == UpscaleKit::kInput);
    kit.nextFrame();
    kit.params.inputContent = { smallest, smallest - 1 };
    kit.upscale();
    CHECK(state.scaler->inputContentWidth() == smallest);
    CHECK(state.scaler->inputContentHeight() == smallest);
    // MetalFX accepts both under the validation layer.
    submitAndWait(kit);
    destroyDevice(kit.kit.device);
}
