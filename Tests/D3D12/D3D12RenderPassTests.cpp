//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12RenderPassTests.cpp
/// @brief Tests D3D12 render passes: their native calls, load, store and resolve operations, the resolve barriers and counts.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/ScopeRecord.h"
#include "D3D12Common.h"
#include "D3D12Device.h"
#include "D3D12ListState.h"
#include "D3D12Textures.h"
#include "Support/D3D12/D3D12Readback.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The width and height of the textures the cases render to.
    constexpr uint32_t kSize = 4;

    /// The samples of a multisampled attachment.
    constexpr uint32_t kSamples = 4;

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
    /// lives.
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

    //==================================================================================================================
    /// Returns a new device with the default description.
    Device makeDevice()
    {
        Result<Device> created = createDevice();
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Returns a new fence at zero on `device`, labelled `label`.
    Fence makeFence(Device device, std::string_view label)
    {
        Result<Fence> created = device.createFence(0, label);
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Returns a new `kSize` by `kSize` 2D texture of `format`, `samples` and `usage` on `device`, labelled `label`.
    Texture makeTexture(Device device, Format format, uint32_t samples, TextureUsage usage, std::string_view label)
    {
        Result<Texture> created = device.createTexture({ .format = format, .extent = { kSize, kSize, 1 }, .samples = samples, .usage = usage, .label = label });
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Returns a new single-sample RGBA8 render target on `device`, labelled `label`.
    Texture makeColor(Device device, std::string_view label)
    {
        return makeTexture(device, Format::RGBA8Unorm, 1, TextureUsage::RenderTarget, label);
    }

    //==================================================================================================================
    /// Returns a new multisampled RGBA8 render target on `device`, labelled `label`.
    Texture makeMultisample(Device device, std::string_view label)
    {
        return makeTexture(device, Format::RGBA8Unorm, kSamples, TextureUsage::RenderTarget, label);
    }

    //==================================================================================================================
    /// Returns the payload of `texture`, a live texture of `device`.
    const d3d12::TexturePayload& payloadOf(Device device, Texture texture)
    {
        const auto* payload = static_cast<const d3d12::TexturePayload*>(DeviceImpl::of(device).core.pool<Texture>().native(texture));
        REQUIRE(payload != nullptr);
        REQUIRE(payload->resource != nullptr);
        return *payload;
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
    /// Returns the native barrier commands `list` has recorded since its acquisition.
    uint64_t nativeBarriers(CommandList list)
    {
        return core::coreOf(list).counts.nativeBarriers;
    }

    //==================================================================================================================
    /// Returns the render passes `list` has begun since its acquisition.
    uint64_t passesBegun(CommandList list)
    {
        return core::coreOf(list).counts.encodersOpened;
    }

    /// The stage and access of one use of a texture subresource.
    struct Use
    {
        Stage stage = Stage::None;    ///< The stages the use runs in.
        Access access = Access::None; ///< What the use does to memory.
    };

    //==================================================================================================================
    /// Returns the use of a subresource in `layout` that a barrier into the layout names as its destination and a barrier out
    /// of it as its source. `ColorAttachment` names the color-attachment and resolve stages with color-attachment writes, the
    /// render-target access that layout admits; `DepthStencilWrite` names depth-stencil reads and writes, and `CopySrc` copy
    /// reads.
    Use useOf(Layout layout)
    {
        switch (layout)
        {
        case Layout::ColorAttachment: return { Stage::ColorAttachment | Stage::Resolve, Access::ColorAttachmentWrite };
        case Layout::DepthStencilWrite: return { Stage::DepthStencil, Access::DepthStencilRead | Access::DepthStencilWrite };
        case Layout::CopySrc: return { Stage::Copy, Access::CopyRead };
        default: return {};
        }
    }

    //==================================================================================================================
    /// Returns a barrier that moves every subresource of `texture` from `from` to `to`, waiting for the use `from` permits
    /// and holding back the use `to` permits; a barrier from `Undefined` waits for nothing.
    TextureBarrier transition(Texture texture, Layout from, Layout to)
    {
        const Use before = useOf(from);
        const Use after = useOf(to);
        return { .texture = texture,
                 .srcStage = before.stage,
                 .srcAccess = before.access,
                 .dstStage = after.stage,
                 .dstAccess = after.access,
                 .oldLayout = from,
                 .newLayout = to };
    }

    //==================================================================================================================
    /// Records one batch on `list` moving each texture of `textures` from `from` to `to`.
    void transitionAll(CommandList list, std::span<const Texture> textures, Layout from, Layout to)
    {
        std::vector<TextureBarrier> barriers;
        for (const Texture texture : textures)
        {
            barriers.push_back(transition(texture, from, to));
        }
        list.barrier({}, {}, barriers);
    }

    //==================================================================================================================
    /// Returns a pass over `colors` and `depth` that renders to the whole of a `kSize` by `kSize` attachment.
    RenderPassDesc passOf(std::span<const ColorAttachment> colors, const DepthAttachment& depth = {})
    {
        return RenderPassDesc{ .colors = colors, .depth = depth, .area = { 0, 0, kSize, kSize }, .label = "test.pass" };
    }

    //==================================================================================================================
    /// Ends `list`, a Graphics list of `device`, submits it signalling `value` on `done`, and waits for the value.
    void submitAndWait(Device device, CommandList list, Fence done, uint64_t value)
    {
        list.end();
        const std::array<FenceValue, 1> signal{ FenceValue{ done, value } };
        device.queue(QueueType::Graphics).submit(std::span(&list, 1), {}, signal);
        REQUIRE(device.wait({ done, value }, UINT64_MAX));
    }

    //==================================================================================================================
    /// Returns `kSize * kSize` copies of the RGBA8 texel `texel`.
    std::vector<std::byte> filled(const std::array<uint8_t, 4>& texel)
    {
        std::vector<std::byte> bytes;
        for (uint32_t i = 0; i < kSize * kSize; ++i)
        {
            for (const uint8_t channel : texel)
            {
                bytes.push_back(static_cast<std::byte>(channel));
            }
        }
        return bytes;
    }
} // namespace

//======================================================================================================================
TEST_CASE(
    "a pass without a resolve costs BeginRenderPass, EndRenderPass, the scope and the first pass's two starting values; a second pass on the list sends no "
    "starting value",
    "[d3d12]"
)
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Texture color = makeColor(device, "test.color");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.passes");
    d3d12::ListState& state = stateOf(list);
    // The native list's dynamic state is unknown after its Reset.
    CHECK(!state.dynamic.stencilReference.has_value());
    CHECK(!state.dynamic.blendFactor.has_value());
    transitionAll(list, std::array{ color }, Layout::Undefined, Layout::ColorAttachment);
    const std::array colors{ ColorAttachment{ .texture = color, .load = LoadOp::Clear, .clear = { 0.0f, 1.0f, 0.0f, 1.0f } } };
    uint64_t before = recordCalls(list);
    const uint64_t barriers = nativeBarriers(list);
    // BeginEvent and EndQuery for the pass's scope, BeginRenderPass, OMSetStencilRef and OMSetBlendFactor.
    list.beginRenderPass(passOf(colors));
    CHECK(recordCalls(list) - before == 5u);
    CHECK(state.dynamic.stencilReference == uint8_t{ 0 });
    CHECK(state.dynamic.blendFactor == std::array<float, 4>{});
    const core::ScopeRecord& scope = core::coreOf(list).scopeLog.back();
    CHECK(scope.label == "test.pass");
    CHECK(scope.timestamp == ((state.ordinal << 16) | 0u));
    CHECK(state.timestamps.open == std::vector<uint32_t>{ 0 });
    // The target's view, its clear and its store.
    REQUIRE(state.pass.targetCount == 1u);
    const D3D12_RENDER_PASS_RENDER_TARGET_DESC& target = state.pass.targets[0];
    CHECK(target.cpuDescriptor.ptr == d3d12::targetView(payloadOf(device, color), 0, 0).ptr);
    CHECK(target.BeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR);
    CHECK(target.BeginningAccess.Clear.ClearValue.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(target.BeginningAccess.Clear.ClearValue.Color[1] == 1.0f);
    CHECK(target.EndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE);
    // EndRenderPass, then EndQuery and EndEvent for the pass's scope.
    before = recordCalls(list);
    list.endRenderPass();
    CHECK(recordCalls(list) - before == 3u);
    CHECK(state.timestamps.open.empty());
    // The second pass finds the starting values held; its recorded depth bias returns to zero without a native call.
    state.dynamic.depthBias = { 1.0f, 2.0f, 3.0f };
    const std::array again{ ColorAttachment{ .texture = color, .load = LoadOp::Load, .store = StoreOp::DontCare } };
    before = recordCalls(list);
    list.beginRenderPass(passOf(again));
    CHECK(recordCalls(list) - before == 3u);
    CHECK(state.dynamic.depthBias == std::array<float, 3>{});
    CHECK(state.pass.targets[0].BeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE);
    CHECK(state.pass.targets[0].EndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD);
    list.endRenderPass();
    // A pass without a resolve target records no barrier.
    CHECK(nativeBarriers(list) == barriers);
    submitAndWait(device, list, done, 1);
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(color);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a pass with a resolve target adds one barrier before and one after, and keeps its parameters alive until EndRenderPass", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Texture multisample = makeMultisample(device, "test.multisample");
    const Texture resolve = makeColor(device, "test.resolve");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.resolve");
    d3d12::ListState& state = stateOf(list);
    transitionAll(list, std::array{ multisample, resolve }, Layout::Undefined, Layout::ColorAttachment);
    const std::array colors{ ColorAttachment{ .texture = multisample, .load = LoadOp::Clear, .resolve = resolve } };
    uint64_t before = recordCalls(list);
    uint64_t barriers = nativeBarriers(list);
    // BeginEvent, EndQuery, the resolve barrier, BeginRenderPass and the two starting values.
    list.beginRenderPass(passOf(colors));
    CHECK(recordCalls(list) - before == 6u);
    CHECK(nativeBarriers(list) - barriers == 1u);
    REQUIRE(state.pass.resolveCount == 1u);
    const D3D12_TEXTURE_BARRIER& barrier = state.pass.resolveBarriers[0];
    CHECK(barrier.pResource == payloadOf(device, resolve).resource);
    CHECK(barrier.SyncBefore == D3D12_BARRIER_SYNC_RENDER_TARGET);
    CHECK(barrier.SyncAfter == D3D12_BARRIER_SYNC_RESOLVE);
    CHECK(barrier.AccessBefore == D3D12_BARRIER_ACCESS_RENDER_TARGET);
    CHECK(barrier.AccessAfter == D3D12_BARRIER_ACCESS_RESOLVE_DEST);
    CHECK(barrier.LayoutBefore == D3D12_BARRIER_LAYOUT_RENDER_TARGET);
    CHECK(barrier.LayoutAfter == D3D12_BARRIER_LAYOUT_RESOLVE_DEST);
    CHECK(barrier.Subresources.IndexOrFirstMipLevel == 0u);
    CHECK(barrier.Subresources.NumMipLevels == 1u);
    CHECK(barrier.Subresources.FirstArraySlice == 0u);
    CHECK(barrier.Subresources.NumArraySlices == 1u);
    const D3D12_RENDER_PASS_ENDING_ACCESS& ending = state.pass.targets[0].EndingAccess;
    REQUIRE(ending.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE);
    CHECK(ending.Resolve.pSrcResource == payloadOf(device, multisample).resource);
    CHECK(ending.Resolve.pDstResource == payloadOf(device, resolve).resource);
    CHECK(ending.Resolve.Format == DXGI_FORMAT_R8G8B8A8_UNORM);
    CHECK(ending.Resolve.ResolveMode == D3D12_RESOLVE_MODE_AVERAGE);
    CHECK(ending.Resolve.PreserveResolveSource == TRUE);
    REQUIRE(ending.Resolve.SubresourceCount == 1u);
    // The parameters the native pass points at are the list's own and stay where they are until EndRenderPass.
    const D3D12_RENDER_PASS_ENDING_ACCESS_RESOLVE_SUBRESOURCE_PARAMETERS* parameters = ending.Resolve.pSubresourceParameters;
    CHECK(parameters == &state.pass.resolves[0]);
    CHECK(parameters->SrcSubresource == 0u);
    CHECK(parameters->DstSubresource == 0u);
    CHECK(parameters->DstX == 0u);
    CHECK(parameters->DstY == 0u);
    CHECK(parameters->SrcRect.left == 0);
    CHECK(parameters->SrcRect.top == 0);
    CHECK(parameters->SrcRect.right == static_cast<LONG>(kSize));
    CHECK(parameters->SrcRect.bottom == static_cast<LONG>(kSize));
    before = recordCalls(list);
    barriers = nativeBarriers(list);
    // EndRenderPass, the barrier back, EndQuery and EndEvent.
    list.endRenderPass();
    CHECK(recordCalls(list) - before == 4u);
    CHECK(nativeBarriers(list) - barriers == 1u);
    CHECK(state.pass.targets[0].EndingAccess.Resolve.pSubresourceParameters == parameters);
    CHECK(barrier.SyncBefore == D3D12_BARRIER_SYNC_RESOLVE);
    CHECK(barrier.SyncAfter == D3D12_BARRIER_SYNC_RENDER_TARGET);
    CHECK(barrier.AccessBefore == D3D12_BARRIER_ACCESS_RESOLVE_DEST);
    CHECK(barrier.AccessAfter == D3D12_BARRIER_ACCESS_RENDER_TARGET);
    CHECK(barrier.LayoutBefore == D3D12_BARRIER_LAYOUT_RESOLVE_DEST);
    CHECK(barrier.LayoutAfter == D3D12_BARRIER_LAYOUT_RENDER_TARGET);
    submitAndWait(device, list, done, 1);
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(multisample);
    device.destroy(resolve);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a pass whose resolve store is DontCare resolves without preserving the source", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Texture multisample = makeMultisample(device, "test.multisample");
    const Texture resolve = makeColor(device, "test.resolve");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.resolve");
    const d3d12::ListState& state = stateOf(list);
    transitionAll(list, std::array{ multisample, resolve }, Layout::Undefined, Layout::ColorAttachment);
    const std::array colors{ ColorAttachment{ .texture = multisample, .load = LoadOp::DontCare, .store = StoreOp::DontCare, .resolve = resolve } };
    list.beginRenderPass(passOf(colors));
    CHECK(state.pass.targets[0].BeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_DISCARD);
    const D3D12_RENDER_PASS_ENDING_ACCESS& ending = state.pass.targets[0].EndingAccess;
    CHECK(ending.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_RESOLVE);
    CHECK(ending.Resolve.PreserveResolveSource == FALSE);
    list.endRenderPass();
    submitAndWait(device, list, done, 1);
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(multisample);
    device.destroy(resolve);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a D32Float attachment uses NO_ACCESS for stencil; a D32FloatS8 attachment uses its stencil ops", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Texture depth = makeTexture(device, Format::D32Float, 1, TextureUsage::DepthStencil, "test.depth");
    const Texture stencil = makeTexture(device, Format::D32FloatS8, 1, TextureUsage::DepthStencil, "test.stencil");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.depth");
    const d3d12::ListState& state = stateOf(list);
    transitionAll(list, std::array{ depth, stencil }, Layout::Undefined, Layout::DepthStencilWrite);
    // A depth-only pass: no render target.
    list.beginRenderPass(passOf({}, DepthAttachment{ .texture = depth, .load = LoadOp::Clear, .clearDepth = 0.375f, .stencilLoad = LoadOp::Clear }));
    CHECK(state.pass.targetCount == 0u);
    REQUIRE(state.pass.hasDepth);
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC& plain = state.pass.depth;
    CHECK(plain.cpuDescriptor.ptr == d3d12::targetView(payloadOf(device, depth), 0, 0).ptr);
    CHECK(plain.DepthBeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR);
    CHECK(plain.DepthBeginningAccess.Clear.ClearValue.Format == DXGI_FORMAT_D32_FLOAT);
    CHECK(plain.DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth == 0.375f);
    CHECK(plain.DepthEndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE);
    // The format has no stencil, so the stencil operations are ignored.
    CHECK(plain.StencilBeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_NO_ACCESS);
    CHECK(plain.StencilEndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_NO_ACCESS);
    list.endRenderPass();
    list.beginRenderPass(passOf(
        {},
        DepthAttachment{ .texture = stencil,
                         .load = LoadOp::Clear,
                         .store = StoreOp::DontCare,
                         .clearDepth = 1.0f,
                         .stencilLoad = LoadOp::Clear,
                         .stencilStore = StoreOp::Store,
                         .clearStencil = 7 }
    ));
    REQUIRE(state.pass.hasDepth);
    const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC& combined = state.pass.depth;
    CHECK(combined.cpuDescriptor.ptr == d3d12::targetView(payloadOf(device, stencil), 0, 0).ptr);
    CHECK(combined.DepthBeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR);
    CHECK(combined.DepthBeginningAccess.Clear.ClearValue.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
    CHECK(combined.DepthBeginningAccess.Clear.ClearValue.DepthStencil.Depth == 1.0f);
    CHECK(combined.DepthEndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_DISCARD);
    CHECK(combined.StencilBeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR);
    CHECK(combined.StencilBeginningAccess.Clear.ClearValue.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
    CHECK(combined.StencilBeginningAccess.Clear.ClearValue.DepthStencil.Stencil == uint8_t{ 7 });
    CHECK(combined.StencilEndingAccess.Type == D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE);
    list.endRenderPass();
    // Stencil loaded and depth not cleared: preserve both.
    list.beginRenderPass(passOf({}, DepthAttachment{ .texture = stencil, .stencilLoad = LoadOp::Load, .stencilStore = StoreOp::Store }));
    CHECK(state.pass.depth.DepthBeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE);
    CHECK(state.pass.depth.StencilBeginningAccess.Type == D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_PRESERVE);
    list.endRenderPass();
    submitAndWait(device, list, done, 1);
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(depth);
    device.destroy(stencil);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("encodersOpened counts passes begun", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Texture color = makeColor(device, "test.color");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.count");
    transitionAll(list, std::array{ color }, Layout::Undefined, Layout::ColorAttachment);
    const std::array colors{ ColorAttachment{ .texture = color, .load = LoadOp::Clear } };
    CHECK(passesBegun(list) == 0u);
    list.beginRenderPass(passOf(colors));
    // The pass counts when it begins.
    CHECK(passesBegun(list) == 1u);
    list.endRenderPass();
    CHECK(passesBegun(list) == 1u);
    list.beginRenderPass(passOf(colors));
    list.endRenderPass();
    CHECK(passesBegun(list) == 2u);
    const Counters before = device.counters();
    submitAndWait(device, list, done, 1);
    // The list's counts fold into the device's at submission.
    CHECK(device.counters().encodersOpened - before.encodersOpened == 2u);
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(color);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a resolve target resolved by two consecutive passes and then read raises no GBV message and holds the second clear", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Texture multisample = makeMultisample(device, "test.multisample");
    const Texture resolve = makeColor(device, "test.resolve");
    const std::array targets{ multisample, resolve };
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.twice");
    transitionAll(list, targets, Layout::Undefined, Layout::ColorAttachment);
    const std::array first{ ColorAttachment{ .texture = multisample, .load = LoadOp::Clear, .clear = { 0.0f, 1.0f, 0.0f, 1.0f }, .resolve = resolve } };
    list.beginRenderPass(passOf(first));
    list.endRenderPass();
    // Each pass's hidden transitions leave the resolve target in ColorAttachment, the layout this barrier names.
    transitionAll(list, targets, Layout::ColorAttachment, Layout::ColorAttachment);
    const std::array second{ ColorAttachment{
        .texture = multisample, .load = LoadOp::Clear, .store = StoreOp::DontCare, .clear = { 1.0f, 0.0f, 0.2f, 0.6f }, .resolve = resolve } };
    list.beginRenderPass(passOf(second));
    list.endRenderPass();
    transitionAll(list, std::array{ resolve }, Layout::ColorAttachment, Layout::CopySrc);
    submitAndWait(device, list, done, 1);
    // Four equal samples average exactly: 0.2 and 0.6 are 51 and 153 out of 255.
    CHECK(
        test::readNativeTexture(DeviceImpl::of(device), payloadOf(device, resolve).resource, 0, D3D12_BARRIER_LAYOUT_COPY_SOURCE) == filled({ 255, 0, 51, 153 })
    );
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(multisample);
    device.destroy(resolve);
    device.destroy(done);
    destroyDevice(device);
}
