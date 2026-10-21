//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12BarrierTests.cpp
/// @brief Tests the lowering of barrier batches to D3D12 enhanced barriers: sync, access, layout, ranges and the native call.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Core/Records.h"
#include "D3D12Addr.h"
#include "D3D12Barriers.h"
#include "D3D12Common.h"
#include "D3D12Device.h"
#include "D3D12ListState.h"
#include "D3D12Textures.h"
#include "Support/D3D12/D3D12Readback.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The sync of the `Copy` stage on a Graphics list: copies, the fill kernel and clears through every view kind.
    constexpr D3D12_BARRIER_SYNC kGraphicsCopySync = D3D12_BARRIER_SYNC_COPY | D3D12_BARRIER_SYNC_COMPUTE_SHADING | D3D12_BARRIER_SYNC_RENDER_TARGET |
                                                     D3D12_BARRIER_SYNC_DEPTH_STENCIL | D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW;

    /// The sync of the `Copy` stage on a Compute list, which has no render-target or depth-stencil work.
    constexpr D3D12_BARRIER_SYNC kComputeCopySync =
        D3D12_BARRIER_SYNC_COPY | D3D12_BARRIER_SYNC_COMPUTE_SHADING | D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW;

    /// The sync of the `Copy` stage on a Graphics list on a side with no access, which drops the unordered-access clear.
    constexpr D3D12_BARRIER_SYNC kGraphicsCopyNoAccess =
        D3D12_BARRIER_SYNC_COPY | D3D12_BARRIER_SYNC_COMPUTE_SHADING | D3D12_BARRIER_SYNC_RENDER_TARGET | D3D12_BARRIER_SYNC_DEPTH_STENCIL;

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
        if (severity == MessageSeverity::Error)
        {
            // A debug-layer error aborts the process right after this callback, before the case lists what it collected.
            std::fprintf(stderr, "error: %.*s\n", static_cast<int>(message.size()), message.data());
            std::fflush(stderr);
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

        /// Returns the messages collected so far, and lists them for the log.
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
    /// Returns a new buffer of `desc` on `device`.
    Buffer makeBuffer(Device device, const BufferDesc& desc)
    {
        Result<Buffer> created = device.createBuffer(desc);
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Returns a new texture of `desc` on `device`, holding `initial` when it is not empty.
    Texture makeTexture(Device device, const TextureDesc& desc, std::span<const TextureData> initial = {})
    {
        Result<Texture> created = device.createTexture(desc, initial);
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Returns a new 4x4 RGBA8 texture of `usage` on `device`, labelled `label`.
    Texture makeColor(Device device, TextureUsage usage, std::string_view label)
    {
        return makeTexture(device, { .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = usage, .label = label });
    }

    //==================================================================================================================
    /// Returns the native buffer of `buffer`, a live buffer of `device`.
    ID3D12Resource* resourceOf(Device device, Buffer buffer)
    {
        const auto* payload = static_cast<const d3d12::BufferPayload*>(DeviceImpl::of(device).core.pool<Buffer>().native(buffer));
        REQUIRE(payload != nullptr);
        return payload->resource;
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
    /// Returns the barrier scratch of `list`, which holds the batch it lowered last.
    const d3d12::BarrierScratch& scratchOf(CommandList list)
    {
        return CommandListImpl::of(list).state.barriers;
    }

    //==================================================================================================================
    /// Returns texture barrier `index` of the batch `list` lowered last, failing the case when the batch has fewer.
    const D3D12_TEXTURE_BARRIER& loweredTexture(CommandList list, size_t index)
    {
        const d3d12::BarrierScratch& scratch = scratchOf(list);
        REQUIRE(scratch.textures.size() > index);
        return scratch.textures[index];
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
    /// Returns the six fields of `range`, so two ranges compare and print as arrays.
    std::array<UINT, 6> fieldsOf(const D3D12_BARRIER_SUBRESOURCE_RANGE& range)
    {
        return { range.IndexOrFirstMipLevel, range.NumMipLevels, range.FirstArraySlice, range.NumArraySlices, range.FirstPlane, range.NumPlanes };
    }

    /// The stage and access of one use of a texture subresource.
    struct Use
    {
        Stage stage = Stage::None;    ///< The stages the use runs in.
        Access access = Access::None; ///< What the use does to memory.
    };

    //==================================================================================================================
    /// Returns a use of a subresource in `layout` on a list of `type`, with the accesses the layout permits: a barrier into
    /// the layout names it as its destination and a barrier out of it as its source.
    Use useOf(Layout layout, QueueType type)
    {
        const Stage shader = type == QueueType::Graphics ? Stage::FragmentShader | Stage::ComputeShader : Stage::ComputeShader;
        switch (layout)
        {
        case Layout::General: return { Stage::Copy | Stage::ComputeShader, Access::CopyRead | Access::CopyWrite | Access::ShaderRead };
        case Layout::ShaderRead: return { shader, Access::ShaderRead };
        case Layout::ShaderWrite: return { Stage::ComputeShader, Access::ShaderRead | Access::ShaderWrite };
        case Layout::ColorAttachment: return { Stage::ColorAttachment, Access::ColorAttachmentRead | Access::ColorAttachmentWrite };
        case Layout::DepthStencilWrite: return { Stage::DepthStencil, Access::DepthStencilRead | Access::DepthStencilWrite };
        case Layout::DepthStencilRead: return { Stage::FragmentShader, Access::ShaderRead };
        case Layout::CopySrc: return { Stage::Copy, Access::CopyRead };
        case Layout::CopyDst: return { Stage::Copy, Access::CopyWrite };
        default: return {};
        }
    }

    //==================================================================================================================
    /// Returns a barrier that moves `range` of `texture` from `from` to `to` on a list of `type`, waiting for the uses `from`
    /// permits and holding back those `to` permits; a barrier from `Undefined` waits for nothing.
    TextureBarrier transition(Texture texture, Layout from, Layout to, QueueType type, SubresourceRange range = {})
    {
        const Use before = useOf(from, type);
        const Use after = useOf(to, type);
        return { .texture = texture,
                 .range = range,
                 .srcStage = before.stage,
                 .srcAccess = before.access,
                 .dstStage = after.stage,
                 .dstAccess = after.access,
                 .oldLayout = from,
                 .newLayout = to };
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
    /// Records `barriers` on a new Graphics list of `device`, one batch, and submits it signalling `value` on `done`, which it
    /// waits for: the debug layer then checks each barrier's layout before against the layout the subresources are in.
    void leave(Device device, std::span<const TextureBarrier> barriers, Fence done, uint64_t value)
    {
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.leave");
        list.barrier({}, {}, barriers);
        submitAndWait(device, list, done, value);
    }
} // namespace

//======================================================================================================================
TEST_CASE("a batch of one global, two buffer and three texture barriers is one native Barrier", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Buffer plain = makeBuffer(device, { .size = 256, .label = "test.plain" });
    const Buffer writable = makeBuffer(device, { .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.writable" });
    const std::array<Texture, 3> textures = { makeColor(device, TextureUsage::Sampled | TextureUsage::Storage, "test.copied"),
                                              makeColor(device, TextureUsage::Sampled | TextureUsage::Storage, "test.sampled"),
                                              makeColor(device, TextureUsage::Sampled | TextureUsage::Storage, "test.stored") };
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.batch");
    const uint64_t calls = recordCalls(list);
    const std::array<GlobalBarrier, 1> globals = { GlobalBarrier{ Stage::Copy, Access::CopyWrite, Stage::ComputeShader, Access::ShaderRead } };
    const std::array<BufferBarrier, 2> buffers = {
        BufferBarrier{
            .buffer = plain, .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::ComputeShader, .dstAccess = Access::ShaderRead },
        BufferBarrier{ .buffer = writable,
                       .offset = 64,
                       .size = 128,
                       .srcStage = Stage::ComputeShader,
                       .srcAccess = Access::ShaderWrite,
                       .dstStage = Stage::Copy,
                       .dstAccess = Access::CopyRead },
    };
    const std::array<TextureBarrier, 3> entering = { transition(textures[0], Layout::Undefined, Layout::CopyDst, QueueType::Graphics),
                                                     transition(textures[1], Layout::Undefined, Layout::ShaderRead, QueueType::Graphics),
                                                     transition(textures[2], Layout::Undefined, Layout::ShaderWrite, QueueType::Graphics) };
    list.barrier(globals, buffers, entering);
    CHECK(recordCalls(list) - calls == 1u);
    CHECK(nativeBarriers(list) == 1u);
    const d3d12::BarrierScratch& scratch = scratchOf(list);
    REQUIRE(scratch.globals.size() == 1u);
    REQUIRE(scratch.buffers.size() == 2u);
    REQUIRE(scratch.textures.size() == 3u);
    CHECK(scratch.buffers[0].pResource == resourceOf(device, plain));
    CHECK(scratch.buffers[1].pResource == resourceOf(device, writable));
    // The range of the second buffer barrier is widened to the whole buffer.
    CHECK(scratch.buffers[1].Offset == 0u);
    CHECK(scratch.buffers[1].Size == UINT64_MAX);
    for (size_t i = 0; i < textures.size(); ++i)
    {
        CHECK(scratch.textures[i].pResource == payloadOf(device, textures[i]).resource);
        CHECK(scratch.textures[i].LayoutAfter == d3d12::layoutOf(entering[i].newLayout));
    }
    submitAndWait(device, list, done, 1);
    const std::array<TextureBarrier, 3> leaving = { transition(textures[0], Layout::CopyDst, Layout::CopySrc, QueueType::Graphics),
                                                    transition(textures[1], Layout::ShaderRead, Layout::CopySrc, QueueType::Graphics),
                                                    transition(textures[2], Layout::ShaderWrite, Layout::CopySrc, QueueType::Graphics) };
    leave(device, leaving, done, 2);
    device.endFrame();
    CHECK(messages.count() == 0u);
    for (const Texture texture : textures)
    {
        device.destroy(texture);
    }
    device.destroy(plain);
    device.destroy(writable);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an empty batch records nothing", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.empty");
    const uint64_t calls = recordCalls(list);
    list.barrier({}, {}, {});
    CHECK(recordCalls(list) == calls);
    CHECK(nativeBarriers(list) == 0u);
    submitAndWait(device, list, done, 1);
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the stage table lowers each Stage as chapter 09 says on a Graphics list", "[d3d12]")
{
    constexpr QueueType kGraphics = QueueType::Graphics;
    CHECK(d3d12::syncOf(Stage::None, kGraphics) == D3D12_BARRIER_SYNC_NONE);
    CHECK(d3d12::syncOf(Stage::IndirectArgs, kGraphics) == D3D12_BARRIER_SYNC_EXECUTE_INDIRECT);
    CHECK(d3d12::syncOf(Stage::IndexInput, kGraphics) == D3D12_BARRIER_SYNC_INDEX_INPUT);
    CHECK(d3d12::syncOf(Stage::VertexShader, kGraphics) == D3D12_BARRIER_SYNC_VERTEX_SHADING);
    CHECK(d3d12::syncOf(Stage::TaskShader, kGraphics) == D3D12_BARRIER_SYNC_VERTEX_SHADING);
    CHECK(d3d12::syncOf(Stage::MeshShader, kGraphics) == D3D12_BARRIER_SYNC_VERTEX_SHADING);
    CHECK(d3d12::syncOf(Stage::FragmentShader, kGraphics) == D3D12_BARRIER_SYNC_PIXEL_SHADING);
    CHECK(d3d12::syncOf(Stage::ComputeShader, kGraphics) == D3D12_BARRIER_SYNC_COMPUTE_SHADING);
    CHECK(d3d12::syncOf(Stage::ColorAttachment, kGraphics) == D3D12_BARRIER_SYNC_RENDER_TARGET);
    CHECK(d3d12::syncOf(Stage::DepthStencil, kGraphics) == D3D12_BARRIER_SYNC_DEPTH_STENCIL);
    CHECK(d3d12::syncOf(Stage::Copy, kGraphics) == kGraphicsCopySync);
    CHECK(d3d12::syncOf(Stage::Resolve, kGraphics) == (D3D12_BARRIER_SYNC_RENDER_TARGET | D3D12_BARRIER_SYNC_RESOLVE));
    CHECK(d3d12::syncOf(Stage::AllGraphics, kGraphics) == (D3D12_BARRIER_SYNC_DRAW | D3D12_BARRIER_SYNC_EXECUTE_INDIRECT | D3D12_BARRIER_SYNC_RESOLVE));
    CHECK(d3d12::syncOf(Stage::All, kGraphics) == D3D12_BARRIER_SYNC_ALL);
    // A combination lowers to the union of its bits' rows.
    CHECK(d3d12::syncOf(Stage::FragmentShader | Stage::ComputeShader, kGraphics) == (D3D12_BARRIER_SYNC_PIXEL_SHADING | D3D12_BARRIER_SYNC_COMPUTE_SHADING));
    CHECK(
        d3d12::syncOf(Stage::AllGraphics | Stage::ComputeShader, kGraphics) ==
        (D3D12_BARRIER_SYNC_DRAW | D3D12_BARRIER_SYNC_EXECUTE_INDIRECT | D3D12_BARRIER_SYNC_RESOLVE | D3D12_BARRIER_SYNC_COMPUTE_SHADING)
    );
    CHECK(d3d12::copyStageSync(kGraphics) == kGraphicsCopySync);
}

//======================================================================================================================
TEST_CASE("Copy drops render-target and depth-stencil sync on a Compute list and is SYNC_COPY alone on a Copy list", "[d3d12]")
{
    CHECK(d3d12::syncOf(Stage::Copy, QueueType::Compute) == kComputeCopySync);
    CHECK(d3d12::copyStageSync(QueueType::Compute) == kComputeCopySync);
    CHECK(d3d12::syncOf(Stage::Copy, QueueType::Copy) == D3D12_BARRIER_SYNC_COPY);
    CHECK(d3d12::copyStageSync(QueueType::Copy) == D3D12_BARRIER_SYNC_COPY);
    // The other stages a Compute or Copy list may name lower as on a Graphics list.
    CHECK(
        d3d12::syncOf(Stage::IndirectArgs | Stage::ComputeShader, QueueType::Compute) ==
        (D3D12_BARRIER_SYNC_EXECUTE_INDIRECT | D3D12_BARRIER_SYNC_COMPUTE_SHADING)
    );
    CHECK(d3d12::syncOf(Stage::All, QueueType::Compute) == D3D12_BARRIER_SYNC_ALL);
    CHECK(d3d12::syncOf(Stage::All, QueueType::Copy) == D3D12_BARRIER_SYNC_ALL);
    CHECK(d3d12::syncOf(Stage::None, QueueType::Copy) == D3D12_BARRIER_SYNC_NONE);
}

//======================================================================================================================
TEST_CASE("CopyWrite adds unordered access only on global barriers and ShaderWrite buffers", "[d3d12]")
{
    using d3d12::AccessTarget;
    constexpr D3D12_BARRIER_ACCESS kFill = D3D12_BARRIER_ACCESS_COPY_DEST | D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
    CHECK(d3d12::accessOf(Access::CopyWrite, AccessTarget::Global, Layout::Undefined) == kFill);
    CHECK(d3d12::accessOf(Access::CopyWrite, AccessTarget::WritableBuffer, Layout::Undefined) == kFill);
    CHECK(d3d12::accessOf(Access::CopyWrite, AccessTarget::Buffer, Layout::Undefined) == D3D12_BARRIER_ACCESS_COPY_DEST);
    CHECK(d3d12::accessOf(Access::CopyWrite, AccessTarget::Texture, Layout::CopyDst) == D3D12_BARRIER_ACCESS_COPY_DEST);
    CHECK(d3d12::accessOf(Access::CopyWrite, AccessTarget::Texture, Layout::General) == D3D12_BARRIER_ACCESS_COPY_DEST);
    // The rest of the access table.
    CHECK(d3d12::accessOf(Access::None, AccessTarget::Global, Layout::Undefined) == D3D12_BARRIER_ACCESS_NO_ACCESS);
    CHECK(d3d12::accessOf(Access::IndirectArgs, AccessTarget::Buffer, Layout::Undefined) == D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
    CHECK(d3d12::accessOf(Access::IndexRead, AccessTarget::Buffer, Layout::Undefined) == D3D12_BARRIER_ACCESS_INDEX_BUFFER);
    CHECK(d3d12::accessOf(Access::ShaderWrite, AccessTarget::WritableBuffer, Layout::Undefined) == D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    CHECK(d3d12::accessOf(Access::CopyRead, AccessTarget::Global, Layout::Undefined) == D3D12_BARRIER_ACCESS_COPY_SOURCE);
    CHECK(
        d3d12::accessOf(Access::ColorAttachmentRead | Access::ColorAttachmentWrite, AccessTarget::Texture, Layout::ColorAttachment) ==
        D3D12_BARRIER_ACCESS_RENDER_TARGET
    );
    CHECK(
        d3d12::accessOf(Access::DepthStencilRead | Access::DepthStencilWrite, AccessTarget::Texture, Layout::DepthStencilWrite) ==
        (D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ | D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE)
    );
    // A resolve target is a ColorAttachment texture, which admits render-target access alone.
    CHECK(d3d12::accessOf(Access::Resolve, AccessTarget::Texture, Layout::ColorAttachment) == D3D12_BARRIER_ACCESS_RENDER_TARGET);

    // Recorded on a Graphics list and on a Copy list, where no fill runs.
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Fence copied = makeFence(device, "test.copied");
    const std::array<Buffer, 2> graphicsBuffers = { makeBuffer(device, { .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.writable" }),
                                                    makeBuffer(device, { .size = 256, .label = "test.plain" }) };
    const std::array<Buffer, 2> copyBuffers = { makeBuffer(device, { .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.copyWritable" }),
                                                makeBuffer(device, { .size = 256, .label = "test.copyPlain" }) };
    const std::array<GlobalBarrier, 1> globals = { GlobalBarrier{ Stage::Copy, Access::CopyWrite, Stage::Copy, Access::CopyRead } };
    auto buffersFor = [](const std::array<Buffer, 2>& targets) {
        std::array<BufferBarrier, 2> barriers = {};
        for (size_t i = 0; i < targets.size(); ++i)
        {
            barriers[i] = {
                .buffer = targets[i], .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead
            };
        }
        return barriers;
    };
    device.beginFrame();
    CommandList graphics = device.acquireCommandList(QueueType::Graphics, "test.graphics");
    graphics.barrier(globals, buffersFor(graphicsBuffers), {});
    {
        const d3d12::BarrierScratch& scratch = scratchOf(graphics);
        REQUIRE(scratch.globals.size() == 1u);
        REQUIRE(scratch.buffers.size() == 2u);
        CHECK(scratch.globals[0].AccessBefore == kFill);
        CHECK(scratch.globals[0].AccessAfter == D3D12_BARRIER_ACCESS_COPY_SOURCE);
        CHECK(scratch.buffers[0].AccessBefore == kFill);
        CHECK(scratch.buffers[1].AccessBefore == D3D12_BARRIER_ACCESS_COPY_DEST);
    }
    submitAndWait(device, graphics, done, 1);
    CommandList copy = device.acquireCommandList(QueueType::Copy, "test.copy");
    copy.barrier(globals, buffersFor(copyBuffers), {});
    {
        const d3d12::BarrierScratch& scratch = scratchOf(copy);
        REQUIRE(scratch.globals.size() == 1u);
        REQUIRE(scratch.buffers.size() == 2u);
        CHECK(scratch.globals[0].SyncBefore == D3D12_BARRIER_SYNC_COPY);
        CHECK(scratch.globals[0].AccessBefore == D3D12_BARRIER_ACCESS_COPY_DEST);
        CHECK(scratch.buffers[0].AccessBefore == D3D12_BARRIER_ACCESS_COPY_DEST);
        CHECK(scratch.buffers[1].AccessBefore == D3D12_BARRIER_ACCESS_COPY_DEST);
    }
    copy.end();
    const std::array<FenceValue, 1> signal{ FenceValue{ copied, 1 } };
    device.queue(QueueType::Copy).submit(std::span(&copy, 1), {}, signal);
    device.queue(QueueType::Graphics).submit({}, signal, {});
    device.endFrame();
    REQUIRE(device.wait({ copied, 1 }, UINT64_MAX));
    CHECK(messages.count() == 0u);
    for (const Buffer buffer : graphicsBuffers)
    {
        device.destroy(buffer);
    }
    for (const Buffer buffer : copyBuffers)
    {
        device.destroy(buffer);
    }
    device.destroy(copied);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("ShaderRead of a texture in ShaderWrite is unordered access", "[d3d12]")
{
    using d3d12::AccessTarget;
    CHECK(d3d12::accessOf(Access::ShaderRead, AccessTarget::Texture, Layout::ShaderWrite) == D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    CHECK(d3d12::accessOf(Access::ShaderRead | Access::ShaderWrite, AccessTarget::Texture, Layout::ShaderWrite) == D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    CHECK(d3d12::accessOf(Access::ShaderRead, AccessTarget::Texture, Layout::ShaderRead) == D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
    CHECK(d3d12::accessOf(Access::ShaderRead, AccessTarget::Texture, Layout::General) == D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
    CHECK(d3d12::accessOf(Access::ShaderRead, AccessTarget::Texture, Layout::DepthStencilRead) == D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
    // Constant-buffer reads exist only through addresses, so only global and buffer barriers name them.
    constexpr D3D12_BARRIER_ACCESS kRead = D3D12_BARRIER_ACCESS_SHADER_RESOURCE | D3D12_BARRIER_ACCESS_CONSTANT_BUFFER;
    CHECK(d3d12::accessOf(Access::ShaderRead, AccessTarget::Global, Layout::Undefined) == kRead);
    CHECK(d3d12::accessOf(Access::ShaderRead, AccessTarget::Buffer, Layout::Undefined) == kRead);
    CHECK(d3d12::accessOf(Access::ShaderRead, AccessTarget::WritableBuffer, Layout::Undefined) == kRead);

    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Texture texture = makeColor(device, TextureUsage::Sampled | TextureUsage::Storage, "test.stored");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.storage");
    const std::array<TextureBarrier, 1> into = { transition(texture, Layout::Undefined, Layout::ShaderWrite, QueueType::Graphics) };
    list.barrier({}, {}, into);
    CHECK(loweredTexture(list, 0).AccessAfter == D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    const std::array<TextureBarrier, 1> out = { transition(texture, Layout::ShaderWrite, Layout::ShaderRead, QueueType::Graphics) };
    list.barrier({}, {}, out);
    CHECK(loweredTexture(list, 0).AccessBefore == D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
    CHECK(loweredTexture(list, 0).AccessAfter == D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
    submitAndWait(device, list, done, 1);
    // The native readback starts from the layout the walk claims to end in.
    CHECK(test::readNativeTexture(DeviceImpl::of(device), payloadOf(device, texture).resource, 0, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE).size() == 4u * 4u * 4u);
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(texture);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a texture walks Undefined, CopyDst, CopySrc, ShaderRead, ShaderWrite, General and back on a Graphics list", "[d3d12]")
{
    constexpr std::array<Layout, 10> kWalk = { Layout::Undefined, Layout::CopyDst,     Layout::CopySrc,    Layout::ShaderRead, Layout::ShaderWrite,
                                               Layout::General,   Layout::ShaderWrite, Layout::ShaderRead, Layout::CopySrc,    Layout::CopyDst };
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Texture texture = makeColor(device, TextureUsage::Sampled | TextureUsage::Storage, "test.walker");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.walk");
    const uint64_t calls = recordCalls(list);
    // Back-to-back batches, the first the list's first command and the last its last.
    for (size_t i = 1; i < kWalk.size(); ++i)
    {
        INFO("from layout " << static_cast<int>(kWalk[i - 1]) << " to " << static_cast<int>(kWalk[i]));
        const std::array<TextureBarrier, 1> step = { transition(texture, kWalk[i - 1], kWalk[i], QueueType::Graphics) };
        list.barrier({}, {}, step);
        const D3D12_TEXTURE_BARRIER& lowered = loweredTexture(list, 0);
        CHECK(lowered.LayoutBefore == d3d12::layoutOf(kWalk[i - 1]));
        CHECK(lowered.LayoutAfter == d3d12::layoutOf(kWalk[i]));
        CHECK(lowered.Flags == (i == 1 ? D3D12_TEXTURE_BARRIER_FLAG_DISCARD : D3D12_TEXTURE_BARRIER_FLAG_NONE));
    }
    CHECK(recordCalls(list) - calls == kWalk.size() - 1);
    CHECK(nativeBarriers(list) == kWalk.size() - 1);
    submitAndWait(device, list, done, 1);
    CHECK(test::readNativeTexture(DeviceImpl::of(device), payloadOf(device, texture).resource, 0, D3D12_BARRIER_LAYOUT_COPY_DEST).size() == 4u * 4u * 4u);
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(texture);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a render target walks ColorAttachment and CopyDst, a depth texture DepthStencilWrite and DepthStencilRead", "[d3d12]")
{
    constexpr std::array<Layout, 5> kColorWalk = { Layout::Undefined, Layout::ColorAttachment, Layout::CopyDst, Layout::ColorAttachment, Layout::CopyDst };
    constexpr std::array<Layout, 5> kDepthWalk = {
        Layout::Undefined, Layout::DepthStencilWrite, Layout::DepthStencilRead, Layout::DepthStencilWrite, Layout::DepthStencilRead
    };
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Texture color = makeColor(device, TextureUsage::RenderTarget, "test.color");
    const Texture depth = makeTexture(
        device, { .format = Format::D32Float, .extent = { 4, 4, 1 }, .usage = TextureUsage::DepthStencil | TextureUsage::Sampled, .label = "test.depth" }
    );
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.targets");
    for (size_t i = 1; i < kColorWalk.size(); ++i)
    {
        INFO("color step " << i);
        const std::array<TextureBarrier, 2> step = { transition(color, kColorWalk[i - 1], kColorWalk[i], QueueType::Graphics),
                                                     transition(depth, kDepthWalk[i - 1], kDepthWalk[i], QueueType::Graphics) };
        list.barrier({}, {}, step);
        CHECK(loweredTexture(list, 0).LayoutAfter == d3d12::layoutOf(kColorWalk[i]));
        CHECK(loweredTexture(list, 1).LayoutAfter == d3d12::layoutOf(kDepthWalk[i]));
        CHECK(loweredTexture(list, 1).Subresources.NumPlanes == 1u);
    }
    // DepthStencilRead permits sampling, which the generic shader-resource layout serves.
    CHECK(loweredTexture(list, 1).LayoutAfter == D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
    CHECK(loweredTexture(list, 1).AccessAfter == D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
    CHECK(loweredTexture(list, 1).SyncAfter == D3D12_BARRIER_SYNC_PIXEL_SHADING);
    submitAndWait(device, list, done, 1);
    CHECK(test::readNativeTexture(DeviceImpl::of(device), payloadOf(device, color).resource, 0, D3D12_BARRIER_LAYOUT_COPY_DEST).size() == 4u * 4u * 4u);
    const std::array<TextureBarrier, 1> leaving = { transition(depth, Layout::DepthStencilRead, Layout::DepthStencilWrite, QueueType::Graphics) };
    leave(device, leaving, done, 2);
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(color);
    device.destroy(depth);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a D32FloatS8 texture's barriers cover both planes", "[d3d12]")
{
    constexpr std::array<Layout, 4> kWalk = { Layout::Undefined, Layout::DepthStencilWrite, Layout::DepthStencilRead, Layout::CopyDst };
    Device device = makeDevice();
    const FormatCapabilities caps = device.formatCapabilities(Format::D32FloatS8);
    REQUIRE(caps.depthAttachment);
    REQUIRE(caps.sampled);
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Texture texture = makeTexture(
        device,
        { .format = Format::D32FloatS8, .extent = { 4, 4, 1 }, .mips = 2, .usage = TextureUsage::DepthStencil | TextureUsage::Sampled, .label = "test.stencil" }
    );
    const Texture depth = makeTexture(
        device, { .format = Format::D32Float, .extent = { 4, 4, 1 }, .usage = TextureUsage::DepthStencil | TextureUsage::Sampled, .label = "test.depth" }
    );
    CHECK(fieldsOf(d3d12::rangeOf(payloadOf(device, texture), {})) == std::array<UINT, 6>{ 0, 2, 0, 1, 0, 2 });
    CHECK(fieldsOf(d3d12::rangeOf(payloadOf(device, texture), { .baseMip = 1 })) == std::array<UINT, 6>{ 1, 1, 0, 1, 0, 2 });
    CHECK(fieldsOf(d3d12::rangeOf(payloadOf(device, depth), {})) == std::array<UINT, 6>{ 0, 1, 0, 1, 0, 1 });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.planes");
    for (size_t i = 1; i < kWalk.size(); ++i)
    {
        INFO("step " << i);
        const std::array<TextureBarrier, 1> step = { transition(texture, kWalk[i - 1], kWalk[i], QueueType::Graphics) };
        list.barrier({}, {}, step);
        CHECK(loweredTexture(list, 0).Subresources.FirstPlane == 0u);
        CHECK(loweredTexture(list, 0).Subresources.NumPlanes == 2u);
    }
    submitAndWait(device, list, done, 1);
    // A later barrier out of CopyDst on both planes: the stencil plane is in that layout too.
    const std::array<TextureBarrier, 1> leaving = { transition(texture, Layout::CopyDst, Layout::DepthStencilWrite, QueueType::Graphics) };
    leave(device, leaving, done, 2);
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(texture);
    device.destroy(depth);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Compute list records every stage and layout VU-rojo-sync-0001 and 0007 allow", "[d3d12]")
{
    constexpr std::array<Layout, 6> kWalk = { Layout::Undefined, Layout::CopyDst, Layout::CopySrc, Layout::ShaderRead, Layout::ShaderWrite, Layout::General };
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Fence computed = makeFence(device, "test.computed");
    const Texture texture = makeColor(device, TextureUsage::Sampled | TextureUsage::Storage, "test.walker");
    const Buffer args = makeBuffer(device, { .size = 256, .usage = BufferUsage::IndirectArgs | BufferUsage::ShaderWrite, .label = "test.args" });
    const Buffer writable = makeBuffer(device, { .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.writable" });
    const std::array<GlobalBarrier, 4> globals = {
        GlobalBarrier{ Stage::Copy, Access::CopyWrite, Stage::IndirectArgs, Access::IndirectArgs },
        GlobalBarrier{ Stage::ComputeShader, Access::ShaderWrite, Stage::Copy, Access::CopyRead },
        GlobalBarrier{ Stage::All, Access::ShaderWrite, Stage::All, Access::ShaderRead },
        GlobalBarrier{ Stage::IndirectArgs | Stage::ComputeShader, Access::None, Stage::Copy, Access::None },
    };
    const std::array<BufferBarrier, 2> buffers = {
        BufferBarrier{
            .buffer = args, .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::IndirectArgs, .dstAccess = Access::IndirectArgs },
        BufferBarrier{
            .buffer = writable, .srcStage = Stage::ComputeShader, .srcAccess = Access::ShaderWrite, .dstStage = Stage::All, .dstAccess = Access::CopyRead },
    };
    device.beginFrame();
    CommandList compute = device.acquireCommandList(QueueType::Compute, "test.compute");
    compute.barrier(globals, buffers, {});
    {
        const d3d12::BarrierScratch& scratch = scratchOf(compute);
        REQUIRE(scratch.globals.size() == 4u);
        REQUIRE(scratch.buffers.size() == 2u);
        CHECK(scratch.globals[0].SyncBefore == kComputeCopySync);
        CHECK(scratch.globals[0].AccessBefore == (D3D12_BARRIER_ACCESS_COPY_DEST | D3D12_BARRIER_ACCESS_UNORDERED_ACCESS));
        CHECK(scratch.globals[0].SyncAfter == D3D12_BARRIER_SYNC_EXECUTE_INDIRECT);
        CHECK(scratch.globals[0].AccessAfter == D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
        CHECK(scratch.globals[2].SyncBefore == D3D12_BARRIER_SYNC_ALL);
        CHECK(scratch.globals[3].AccessBefore == D3D12_BARRIER_ACCESS_NO_ACCESS);
        CHECK(scratch.globals[3].AccessAfter == D3D12_BARRIER_ACCESS_NO_ACCESS);
        CHECK(scratch.buffers[0].AccessBefore == (D3D12_BARRIER_ACCESS_COPY_DEST | D3D12_BARRIER_ACCESS_UNORDERED_ACCESS));
        CHECK(scratch.buffers[1].SyncAfter == D3D12_BARRIER_SYNC_ALL);
    }
    for (size_t i = 1; i < kWalk.size(); ++i)
    {
        INFO("step " << i);
        const std::array<TextureBarrier, 1> step = { transition(texture, kWalk[i - 1], kWalk[i], QueueType::Compute) };
        compute.barrier({}, {}, step);
        CHECK(loweredTexture(compute, 0).LayoutAfter == d3d12::layoutOf(kWalk[i]));
    }
    compute.end();
    const std::array<FenceValue, 1> signal{ FenceValue{ computed, 1 } };
    device.queue(QueueType::Compute).submit(std::span(&compute, 1), {}, signal);
    // The graphics queue joins the compute work, then takes the texture out of General, the layout of the handoff.
    CommandList graphics = device.acquireCommandList(QueueType::Graphics, "test.graphics");
    const std::array<TextureBarrier, 1> leaving = { transition(texture, Layout::General, Layout::CopySrc, QueueType::Graphics) };
    graphics.barrier({}, {}, leaving);
    graphics.end();
    const std::array<FenceValue, 1> doneSignal{ FenceValue{ done, 1 } };
    device.queue(QueueType::Graphics).submit(std::span(&graphics, 1), signal, doneSignal);
    REQUIRE(device.wait({ done, 1 }, UINT64_MAX));
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(texture);
    device.destroy(args);
    device.destroy(writable);
    device.destroy(computed);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Copy list records Copy and All on General textures and on buffers", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Fence copied = makeFence(device, "test.copied");
    const std::array<Texture, 2> textures = { makeColor(device, TextureUsage::Sampled, "test.first"), makeColor(device, TextureUsage::Sampled, "test.second") };
    const Buffer plain = makeBuffer(device, { .size = 256, .label = "test.plain" });
    const Buffer writable = makeBuffer(device, { .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.writable" });
    device.beginFrame();
    // A Copy list uses textures in General only, so the graphics queue moves them there first.
    CommandList setup = device.acquireCommandList(QueueType::Graphics, "test.setup");
    const std::array<TextureBarrier, 2> entering = { transition(textures[0], Layout::Undefined, Layout::General, QueueType::Graphics),
                                                     transition(textures[1], Layout::Undefined, Layout::General, QueueType::Graphics) };
    setup.barrier({}, {}, entering);
    submitAndWait(device, setup, done, 1);
    CommandList copy = device.acquireCommandList(QueueType::Copy, "test.copy");
    const std::array<GlobalBarrier, 1> globals = { GlobalBarrier{ Stage::Copy, Access::CopyWrite, Stage::All, Access::CopyRead } };
    const std::array<BufferBarrier, 2> buffers = {
        BufferBarrier{ .buffer = plain, .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead },
        BufferBarrier{ .buffer = writable, .srcStage = Stage::All, .srcAccess = Access::CopyRead, .dstStage = Stage::All, .dstAccess = Access::CopyWrite },
    };
    const std::array<TextureBarrier, 2> general = {
        TextureBarrier{ .texture = textures[0],
                        .srcStage = Stage::Copy,
                        .srcAccess = Access::CopyWrite,
                        .dstStage = Stage::Copy,
                        .dstAccess = Access::CopyRead,
                        .oldLayout = Layout::General,
                        .newLayout = Layout::General },
        TextureBarrier{ .texture = textures[1],
                        .srcStage = Stage::All,
                        .srcAccess = Access::CopyRead,
                        .dstStage = Stage::All,
                        .dstAccess = Access::CopyWrite,
                        .oldLayout = Layout::General,
                        .newLayout = Layout::General },
    };
    copy.barrier(globals, buffers, general);
    {
        const d3d12::BarrierScratch& scratch = scratchOf(copy);
        // A copy queue takes no layout barrier, so each texture barrier joins the global group.
        REQUIRE(scratch.globals.size() == 3u);
        REQUIRE(scratch.buffers.size() == 2u);
        CHECK(scratch.textures.empty());
        CHECK(scratch.globals[0].SyncBefore == D3D12_BARRIER_SYNC_COPY);
        CHECK(scratch.globals[0].SyncAfter == D3D12_BARRIER_SYNC_ALL);
        CHECK(scratch.globals[0].AccessBefore == D3D12_BARRIER_ACCESS_COPY_DEST);
        CHECK(scratch.buffers[1].AccessAfter == D3D12_BARRIER_ACCESS_COPY_DEST);
        CHECK(scratch.globals[1].SyncBefore == D3D12_BARRIER_SYNC_COPY);
        CHECK(scratch.globals[1].AccessBefore == D3D12_BARRIER_ACCESS_COPY_DEST);
        CHECK(scratch.globals[1].AccessAfter == D3D12_BARRIER_ACCESS_COPY_SOURCE);
        CHECK(scratch.globals[2].SyncBefore == D3D12_BARRIER_SYNC_ALL);
        CHECK(scratch.globals[2].AccessBefore == D3D12_BARRIER_ACCESS_COPY_SOURCE);
        CHECK(scratch.globals[2].AccessAfter == D3D12_BARRIER_ACCESS_COPY_DEST);
    }
    copy.end();
    const std::array<FenceValue, 1> setupDone{ FenceValue{ done, 1 } };
    const std::array<FenceValue, 1> signal{ FenceValue{ copied, 1 } };
    device.queue(QueueType::Copy).submit(std::span(&copy, 1), setupDone, signal);
    // The graphics queue joins the copy work and takes both textures out of General.
    CommandList graphics = device.acquireCommandList(QueueType::Graphics, "test.graphics");
    const std::array<TextureBarrier, 2> leaving = { transition(textures[0], Layout::General, Layout::CopySrc, QueueType::Graphics),
                                                    transition(textures[1], Layout::General, Layout::CopySrc, QueueType::Graphics) };
    graphics.barrier({}, {}, leaving);
    graphics.end();
    const std::array<FenceValue, 1> doneSignal{ FenceValue{ done, 2 } };
    device.queue(QueueType::Graphics).submit(std::span(&graphics, 1), signal, doneSignal);
    REQUIRE(device.wait({ done, 2 }, UINT64_MAX));
    device.endFrame();
    CHECK(messages.count() == 0u);
    for (const Texture texture : textures)
    {
        device.destroy(texture);
    }
    device.destroy(plain);
    device.destroy(writable);
    device.destroy(copied);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a barrier from Undefined discards and names no earlier access", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Texture texture = makeColor(device, TextureUsage::RenderTarget | TextureUsage::Sampled, "test.target");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.discard");
    // The source stage still orders earlier copies before the layout changes; the source names no access.
    const std::array<TextureBarrier, 1> discard = { TextureBarrier{ .texture = texture,
                                                                    .srcStage = Stage::Copy,
                                                                    .srcAccess = Access::None,
                                                                    .dstStage = Stage::ColorAttachment,
                                                                    .dstAccess = Access::ColorAttachmentWrite,
                                                                    .oldLayout = Layout::Undefined,
                                                                    .newLayout = Layout::ColorAttachment } };
    list.barrier({}, {}, discard);
    {
        const D3D12_TEXTURE_BARRIER& lowered = loweredTexture(list, 0);
        CHECK(lowered.SyncBefore == kGraphicsCopyNoAccess);
        CHECK(lowered.AccessBefore == D3D12_BARRIER_ACCESS_NO_ACCESS);
        CHECK(lowered.LayoutBefore == D3D12_BARRIER_LAYOUT_UNDEFINED);
        CHECK(lowered.Flags == D3D12_TEXTURE_BARRIER_FLAG_DISCARD);
        CHECK(lowered.AccessAfter == D3D12_BARRIER_ACCESS_RENDER_TARGET);
    }
    CHECK(d3d12::accessOf(Access::CopyWrite, d3d12::AccessTarget::Texture, Layout::Undefined) == D3D12_BARRIER_ACCESS_NO_ACCESS);
    const std::array<TextureBarrier, 1> next = { transition(texture, Layout::ColorAttachment, Layout::CopyDst, QueueType::Graphics) };
    list.barrier({}, {}, next);
    CHECK(loweredTexture(list, 0).Flags == D3D12_TEXTURE_BARRIER_FLAG_NONE);
    CHECK(loweredTexture(list, 0).AccessBefore == D3D12_BARRIER_ACCESS_RENDER_TARGET);
    submitAndWait(device, list, done, 1);
    CHECK(test::readNativeTexture(DeviceImpl::of(device), payloadOf(device, texture).resource, 0, D3D12_BARRIER_LAYOUT_COPY_DEST).size() == 4u * 4u * 4u);
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(texture);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a texture created with initial data leaves ShaderRead without a debug-layer message", "[d3d12]")
{
    Device device = makeDevice();
    std::vector<std::byte> texels(4 * 4 * 4);
    for (size_t i = 0; i < texels.size(); ++i)
    {
        texels[i] = static_cast<std::byte>((i * 7 + 3) & 0xFF);
    }
    const TextureData initial{ .bytes = texels, .bytesPerRow = 16, .bytesPerImage = 0 };
    const Texture texture = makeTexture(
        device, { .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled, .label = "test.initial" }, std::span(&initial, 1)
    );
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.initial");
    const std::array<TextureBarrier, 1> leaving = { transition(texture, Layout::ShaderRead, Layout::CopySrc, QueueType::Graphics) };
    list.barrier({}, {}, leaving);
    CHECK(loweredTexture(list, 0).LayoutBefore == D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
    CHECK(loweredTexture(list, 0).AccessBefore == D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
    CHECK(loweredTexture(list, 0).Flags == D3D12_TEXTURE_BARRIER_FLAG_NONE);
    submitAndWait(device, list, done, 1);
    // The texels survive the transition, which does not discard.
    CHECK(test::readNativeTexture(DeviceImpl::of(device), payloadOf(device, texture).resource, 0, D3D12_BARRIER_LAYOUT_COPY_SOURCE) == texels);
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(texture);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a cube array's layer range and a mip range reach exactly those subresources", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Texture cubes = makeTexture(
        device,
        { .type = TextureType::CubeArray,
          .format = Format::RGBA8Unorm,
          .extent = { 8, 8, 1 },
          .mips = 3,
          .layers = 12,
          .usage = TextureUsage::Sampled,
          .label = "test.cubes" }
    );
    const Texture volume = makeTexture(
        device,
        { .type = TextureType::Tex3D, .format = Format::RGBA8Unorm, .extent = { 8, 8, 4 }, .mips = 2, .usage = TextureUsage::Sampled, .label = "test.volume" }
    );
    const d3d12::TexturePayload& cubePayload = payloadOf(device, cubes);
    const d3d12::TexturePayload& volumePayload = payloadOf(device, volume);
    // kAllMips and kAllLayers resolve against a non-zero base.
    const SubresourceRange upper = { .baseMip = 1, .mipCount = kAllMips, .baseLayer = 6, .layerCount = kAllLayers };
    const SubresourceRange lower = { .baseMip = 0, .mipCount = 1, .baseLayer = 3, .layerCount = 2 };
    CHECK(fieldsOf(d3d12::rangeOf(cubePayload, upper)) == std::array<UINT, 6>{ 1, 2, 6, 6, 0, 1 });
    CHECK(fieldsOf(d3d12::rangeOf(cubePayload, lower)) == std::array<UINT, 6>{ 0, 1, 3, 2, 0, 1 });
    CHECK(fieldsOf(d3d12::rangeOf(cubePayload, {})) == std::array<UINT, 6>{ 0, 3, 0, 12, 0, 1 });
    // A 3D texture's depth slices are one array slice.
    const SubresourceRange volumeUpper = { .baseMip = 1 };
    CHECK(fieldsOf(d3d12::rangeOf(volumePayload, volumeUpper)) == std::array<UINT, 6>{ 1, 1, 0, 1, 0, 1 });
    CHECK(fieldsOf(d3d12::rangeOf(volumePayload, {})) == std::array<UINT, 6>{ 0, 2, 0, 1, 0, 1 });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.ranges");
    const std::array<TextureBarrier, 3> entering = { transition(cubes, Layout::Undefined, Layout::CopySrc, QueueType::Graphics, upper),
                                                     transition(cubes, Layout::Undefined, Layout::ShaderRead, QueueType::Graphics, lower),
                                                     transition(volume, Layout::Undefined, Layout::CopyDst, QueueType::Graphics, volumeUpper) };
    list.barrier({}, {}, entering);
    CHECK(fieldsOf(loweredTexture(list, 0).Subresources) == fieldsOf(d3d12::rangeOf(cubePayload, upper)));
    CHECK(fieldsOf(loweredTexture(list, 1).Subresources) == fieldsOf(d3d12::rangeOf(cubePayload, lower)));
    CHECK(fieldsOf(loweredTexture(list, 2).Subresources) == fieldsOf(d3d12::rangeOf(volumePayload, volumeUpper)));
    submitAndWait(device, list, done, 1);
    // Each subresource read back starts from the layout its range reached: mip 1 of layer 6, mip 0 of layer 4, and mip 1
    // of the volume, whose two depth slices it returns.
    DeviceImpl& impl = DeviceImpl::of(device);
    CHECK(test::readNativeTexture(impl, cubePayload.resource, 1 + 6 * 3, D3D12_BARRIER_LAYOUT_COPY_SOURCE).size() == 4u * 4u * 4u);
    CHECK(test::readNativeTexture(impl, cubePayload.resource, 0 + 4 * 3, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE).size() == 8u * 8u * 4u);
    CHECK(test::readNativeTexture(impl, volumePayload.resource, 1, D3D12_BARRIER_LAYOUT_COPY_DEST).size() == 4u * 4u * 2u * 4u);
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(cubes);
    device.destroy(volume);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("buffers in Upload and Readback memory take buffer barriers", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Buffer upload = makeBuffer(device, { .size = 256, .memory = Memory::Upload, .label = "test.upload" });
    const Buffer readback = makeBuffer(device, { .size = 256, .memory = Memory::Readback, .label = "test.readback" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.host");
    const uint64_t calls = recordCalls(list);
    // Each buffer keeps to the accesses its memory serves: reads of upload memory, copy writes into readback memory.
    const std::array<BufferBarrier, 2> buffers = {
        BufferBarrier{
            .buffer = upload, .srcStage = Stage::Copy, .srcAccess = Access::CopyRead, .dstStage = Stage::ComputeShader, .dstAccess = Access::ShaderRead },
        BufferBarrier{ .buffer = readback, .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyWrite },
    };
    list.barrier({}, buffers, {});
    CHECK(recordCalls(list) - calls == 1u);
    REQUIRE(scratchOf(list).buffers.size() == 2u);
    CHECK(scratchOf(list).buffers[0].pResource == resourceOf(device, upload));
    CHECK(scratchOf(list).buffers[1].AccessAfter == D3D12_BARRIER_ACCESS_COPY_DEST);
    submitAndWait(device, list, done, 1);
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(upload);
    device.destroy(readback);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Copy list drops the accesses of Stage::All its queue never performs", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence copied = makeFence(device, "test.copied");
    const Buffer writable = makeBuffer(device, { .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.writable" });
    device.beginFrame();
    CommandList copy = device.acquireCommandList(QueueType::Copy, "test.copy");
    // Stage::All performs every access, so these sides are valid, but a copy queue performs only copy accesses.
    const std::array<GlobalBarrier, 1> globals = { GlobalBarrier{ Stage::All, Access::ShaderRead, Stage::All, Access::CopyRead } };
    const std::array<BufferBarrier, 1> buffers = { BufferBarrier{ .buffer = writable,
                                                                  .srcStage = Stage::All,
                                                                  .srcAccess = Access::ShaderRead | Access::CopyWrite,
                                                                  .dstStage = Stage::All,
                                                                  .dstAccess = Access::IndexRead } };
    copy.barrier(globals, buffers, {});
    {
        const d3d12::BarrierScratch& scratch = scratchOf(copy);
        REQUIRE(scratch.globals.size() == 1u);
        REQUIRE(scratch.buffers.size() == 1u);
        CHECK(scratch.globals[0].SyncBefore == D3D12_BARRIER_SYNC_ALL);
        CHECK(scratch.globals[0].AccessBefore == D3D12_BARRIER_ACCESS_NO_ACCESS);
        CHECK(scratch.globals[0].AccessAfter == D3D12_BARRIER_ACCESS_COPY_SOURCE);
        CHECK(scratch.buffers[0].AccessBefore == D3D12_BARRIER_ACCESS_COPY_DEST);
        CHECK(scratch.buffers[0].AccessAfter == D3D12_BARRIER_ACCESS_NO_ACCESS);
    }
    copy.end();
    const std::array<FenceValue, 1> signal{ FenceValue{ copied, 1 } };
    device.queue(QueueType::Copy).submit(std::span(&copy, 1), {}, signal);
    device.queue(QueueType::Graphics).submit({}, signal, {});
    device.endFrame();
    REQUIRE(device.wait({ copied, 1 }, UINT64_MAX));
    CHECK(messages.count() == 0u);
    device.destroy(writable);
    device.destroy(copied);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Compute list drops the accesses of Stage::All its queue never performs", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence computed = makeFence(device, "test.computed");
    const Buffer args = makeBuffer(device, { .size = 256, .usage = BufferUsage::IndirectArgs | BufferUsage::Index, .label = "test.args" });
    device.beginFrame();
    CommandList compute = device.acquireCommandList(QueueType::Compute, "test.compute");
    // A compute queue performs no render-target, depth-stencil, resolve or index access.
    const std::array<GlobalBarrier, 2> globals = {
        GlobalBarrier{ Stage::All, Access::ColorAttachmentWrite | Access::ShaderWrite, Stage::All, Access::IndexRead | Access::ShaderRead },
        GlobalBarrier{ Stage::All, Access::Resolve, Stage::All, Access::DepthStencilRead },
    };
    const std::array<BufferBarrier, 1> buffers = { BufferBarrier{
        .buffer = args, .srcStage = Stage::All, .srcAccess = Access::IndexRead, .dstStage = Stage::All, .dstAccess = Access::IndirectArgs } };
    compute.barrier(globals, buffers, {});
    {
        const d3d12::BarrierScratch& scratch = scratchOf(compute);
        REQUIRE(scratch.globals.size() == 2u);
        REQUIRE(scratch.buffers.size() == 1u);
        CHECK(scratch.globals[0].AccessBefore == D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
        CHECK(scratch.globals[0].AccessAfter == (D3D12_BARRIER_ACCESS_SHADER_RESOURCE | D3D12_BARRIER_ACCESS_CONSTANT_BUFFER));
        CHECK(scratch.globals[1].SyncBefore == D3D12_BARRIER_SYNC_ALL);
        CHECK(scratch.globals[1].AccessBefore == D3D12_BARRIER_ACCESS_NO_ACCESS);
        CHECK(scratch.globals[1].AccessAfter == D3D12_BARRIER_ACCESS_NO_ACCESS);
        CHECK(scratch.buffers[0].AccessBefore == D3D12_BARRIER_ACCESS_NO_ACCESS);
        CHECK(scratch.buffers[0].AccessAfter == D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT);
    }
    compute.end();
    const std::array<FenceValue, 1> signal{ FenceValue{ computed, 1 } };
    device.queue(QueueType::Compute).submit(std::span(&compute, 1), {}, signal);
    device.queue(QueueType::Graphics).submit({}, signal, {});
    device.endFrame();
    REQUIRE(device.wait({ computed, 1 }, UINT64_MAX));
    CHECK(messages.count() == 0u);
    device.destroy(args);
    device.destroy(computed);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a side's sync keeps only the scopes its accesses run in", "[d3d12]")
{
    constexpr D3D12_BARRIER_SYNC kFillSync = D3D12_BARRIER_SYNC_COPY | D3D12_BARRIER_SYNC_COMPUTE_SHADING | D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW;
    CHECK(d3d12::narrowSync(kGraphicsCopySync, D3D12_BARRIER_ACCESS_COPY_DEST) == D3D12_BARRIER_SYNC_COPY);
    CHECK(d3d12::narrowSync(kGraphicsCopySync, D3D12_BARRIER_ACCESS_COPY_DEST | D3D12_BARRIER_ACCESS_UNORDERED_ACCESS) == kFillSync);
    // A side with no access keeps every scope but the unordered-access clear's, which the layer rejects without one.
    CHECK(d3d12::narrowSync(kGraphicsCopySync, D3D12_BARRIER_ACCESS_NO_ACCESS) == kGraphicsCopyNoAccess);
    CHECK(d3d12::narrowSync(D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW, D3D12_BARRIER_ACCESS_NO_ACCESS) == D3D12_BARRIER_SYNC_ALL);
    CHECK(
        d3d12::narrowSync(kGraphicsCopySync | D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE | D3D12_BARRIER_ACCESS_COPY_SOURCE) ==
        (D3D12_BARRIER_SYNC_COPY | D3D12_BARRIER_SYNC_COMPUTE_SHADING)
    );
    CHECK(
        d3d12::narrowSync(D3D12_BARRIER_SYNC_RENDER_TARGET | D3D12_BARRIER_SYNC_RESOLVE, D3D12_BARRIER_ACCESS_RENDER_TARGET) == D3D12_BARRIER_SYNC_RENDER_TARGET
    );
    CHECK(
        d3d12::narrowSync(D3D12_BARRIER_SYNC_DRAW | D3D12_BARRIER_SYNC_EXECUTE_INDIRECT | D3D12_BARRIER_SYNC_RESOLVE, D3D12_BARRIER_ACCESS_INDEX_BUFFER) ==
        D3D12_BARRIER_SYNC_DRAW
    );
    CHECK(d3d12::narrowSync(D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_ACCESS_COPY_DEST) == D3D12_BARRIER_SYNC_ALL);
    CHECK(d3d12::narrowSync(D3D12_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_ACCESS_SHADER_RESOURCE) == D3D12_BARRIER_SYNC_PIXEL_SHADING);
    // No scope of the sync runs the access: the side widens to every scope rather than name an incompatible one.
    CHECK(d3d12::narrowSync(D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_SHADER_RESOURCE) == D3D12_BARRIER_SYNC_ALL);

    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Texture texture = makeColor(device, TextureUsage::Sampled | TextureUsage::Storage, "test.copied");
    const Buffer plain = makeBuffer(device, { .size = 256, .label = "test.plain" });
    const Buffer writable = makeBuffer(device, { .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.writable" });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.narrow");
    const std::array<BufferBarrier, 2> buffers = {
        BufferBarrier{ .buffer = plain, .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead },
        BufferBarrier{ .buffer = writable, .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead },
    };
    const std::array<TextureBarrier, 1> entering = { transition(texture, Layout::Undefined, Layout::CopyDst, QueueType::Graphics) };
    list.barrier({}, buffers, entering);
    {
        const d3d12::BarrierScratch& scratch = scratchOf(list);
        REQUIRE(scratch.buffers.size() == 2u);
        CHECK(scratch.buffers[0].SyncBefore == D3D12_BARRIER_SYNC_COPY);
        CHECK(scratch.buffers[0].SyncAfter == D3D12_BARRIER_SYNC_COPY);
        // The fill kernel writes a ShaderWrite buffer, so its compute and clear scopes stay.
        CHECK(scratch.buffers[1].SyncBefore == kFillSync);
        CHECK(scratch.buffers[1].SyncAfter == D3D12_BARRIER_SYNC_COPY);
    }
    // From Undefined with no source stage the source takes the destination's scope, a copy write alone.
    CHECK(loweredTexture(list, 0).SyncBefore == D3D12_BARRIER_SYNC_COPY);
    CHECK(loweredTexture(list, 0).SyncAfter == D3D12_BARRIER_SYNC_COPY);
    const std::array<TextureBarrier, 1> leaving = { transition(texture, Layout::CopyDst, Layout::General, QueueType::Graphics) };
    list.barrier({}, {}, leaving);
    CHECK(loweredTexture(list, 0).SyncBefore == D3D12_BARRIER_SYNC_COPY);
    CHECK(loweredTexture(list, 0).SyncAfter == (D3D12_BARRIER_SYNC_COPY | D3D12_BARRIER_SYNC_COMPUTE_SHADING));
    submitAndWait(device, list, done, 1);
    const std::array<TextureBarrier, 1> out = { transition(texture, Layout::General, Layout::CopySrc, QueueType::Graphics) };
    leave(device, out, done, 2);
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(texture);
    device.destroy(plain);
    device.destroy(writable);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a barrier from Undefined after copies keeps the whole Copy scope on a Compute list", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const Fence computed = makeFence(device, "test.computed");
    const Texture texture = makeColor(device, TextureUsage::Sampled | TextureUsage::Storage, "test.discarded");
    device.beginFrame();
    CommandList compute = device.acquireCommandList(QueueType::Compute, "test.compute");
    // The source names the Copy stage but no access: it waits for earlier copies, fills and clears with the wide scope.
    const std::array<TextureBarrier, 1> discard = { TextureBarrier{ .texture = texture,
                                                                    .srcStage = Stage::Copy,
                                                                    .srcAccess = Access::None,
                                                                    .dstStage = Stage::Copy,
                                                                    .dstAccess = Access::CopyWrite,
                                                                    .oldLayout = Layout::Undefined,
                                                                    .newLayout = Layout::CopyDst } };
    compute.barrier({}, {}, discard);
    {
        const D3D12_TEXTURE_BARRIER& lowered = loweredTexture(compute, 0);
        CHECK(lowered.SyncBefore == (D3D12_BARRIER_SYNC_COPY | D3D12_BARRIER_SYNC_COMPUTE_SHADING));
        CHECK(lowered.AccessBefore == D3D12_BARRIER_ACCESS_NO_ACCESS);
        CHECK(lowered.LayoutBefore == D3D12_BARRIER_LAYOUT_UNDEFINED);
        CHECK(lowered.Flags == D3D12_TEXTURE_BARRIER_FLAG_DISCARD);
        CHECK(lowered.SyncAfter == D3D12_BARRIER_SYNC_COPY);
        CHECK(lowered.AccessAfter == D3D12_BARRIER_ACCESS_COPY_DEST);
    }
    compute.end();
    const std::array<FenceValue, 1> signal{ FenceValue{ computed, 1 } };
    device.queue(QueueType::Compute).submit(std::span(&compute, 1), {}, signal);
    // The graphics queue joins the compute work and takes the texture out of CopyDst.
    CommandList graphics = device.acquireCommandList(QueueType::Graphics, "test.graphics");
    const std::array<TextureBarrier, 1> leaving = { transition(texture, Layout::CopyDst, Layout::CopySrc, QueueType::Graphics) };
    graphics.barrier({}, {}, leaving);
    graphics.end();
    const std::array<FenceValue, 1> doneSignal{ FenceValue{ done, 1 } };
    device.queue(QueueType::Graphics).submit(std::span(&graphics, 1), signal, doneSignal);
    REQUIRE(device.wait({ done, 1 }, UINT64_MAX));
    device.endFrame();
    CHECK(messages.count() == 0u);
    device.destroy(texture);
    device.destroy(computed);
    device.destroy(done);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a barrier side with no access waits in a scope the layer accepts", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Fence done = makeFence(device, "test.done");
    const std::array<Texture, 3> textures = { makeColor(device, TextureUsage::Sampled | TextureUsage::Storage, "test.used"),
                                              makeColor(device, TextureUsage::Sampled, "test.latched"),
                                              makeColor(device, TextureUsage::Sampled, "test.ordered") };
    const Buffer buffer = makeBuffer(device, { .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.buffer" });
    device.beginFrame();
    // The textures are touched in an earlier submission first, after which validation rejects SYNC_NONE before them.
    CommandList first = device.acquireCommandList(QueueType::Graphics, "test.first");
    const std::array<TextureBarrier, 3> entering = { transition(textures[0], Layout::Undefined, Layout::CopyDst, QueueType::Graphics),
                                                     transition(textures[1], Layout::Undefined, Layout::CopySrc, QueueType::Graphics),
                                                     transition(textures[2], Layout::Undefined, Layout::General, QueueType::Graphics) };
    first.barrier({}, {}, entering);
    submitAndWait(device, first, done, 1);
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.sourceless");
    const std::array<BufferBarrier, 1> buffers = { BufferBarrier{
        .buffer = buffer, .srcStage = Stage::None, .srcAccess = Access::None, .dstStage = Stage::ComputeShader, .dstAccess = Access::ShaderWrite } };
    // A discard with no source stage, a layout change that names no stage on either side, and one that orders every stage
    // before a copy stage with no access on either side.
    const std::array<TextureBarrier, 3> sourceless = {
        TextureBarrier{ .texture = textures[0],
                        .srcStage = Stage::None,
                        .srcAccess = Access::None,
                        .dstStage = Stage::ComputeShader,
                        .dstAccess = Access::ShaderRead | Access::ShaderWrite,
                        .oldLayout = Layout::Undefined,
                        .newLayout = Layout::ShaderWrite },
        TextureBarrier{ .texture = textures[1],
                        .srcStage = Stage::None,
                        .srcAccess = Access::None,
                        .dstStage = Stage::None,
                        .dstAccess = Access::None,
                        .oldLayout = Layout::CopySrc,
                        .newLayout = Layout::ShaderRead },
        TextureBarrier{ .texture = textures[2],
                        .srcStage = Stage::All,
                        .srcAccess = Access::None,
                        .dstStage = Stage::Copy,
                        .dstAccess = Access::None,
                        .oldLayout = Layout::General,
                        .newLayout = Layout::CopySrc },
    };
    list.barrier({}, buffers, sourceless);
    {
        const d3d12::BarrierScratch& scratch = scratchOf(list);
        REQUIRE(scratch.buffers.size() == 1u);
        CHECK(scratch.buffers[0].SyncBefore == D3D12_BARRIER_SYNC_COMPUTE_SHADING);
        CHECK(scratch.buffers[0].AccessBefore == D3D12_BARRIER_ACCESS_NO_ACCESS);
        CHECK(scratch.buffers[0].SyncAfter == D3D12_BARRIER_SYNC_COMPUTE_SHADING);
    }
    CHECK(loweredTexture(list, 0).SyncBefore == D3D12_BARRIER_SYNC_COMPUTE_SHADING);
    CHECK(loweredTexture(list, 0).AccessBefore == D3D12_BARRIER_ACCESS_NO_ACCESS);
    CHECK(loweredTexture(list, 0).Flags == D3D12_TEXTURE_BARRIER_FLAG_DISCARD);
    // No access in a defined layout needs SYNC_NONE, which validation rejects once the subresource was touched: the side
    // names what the layout admits, in the layout's own scope when it names none.
    CHECK(loweredTexture(list, 1).SyncBefore == D3D12_BARRIER_SYNC_COPY);
    CHECK(loweredTexture(list, 1).AccessBefore == D3D12_BARRIER_ACCESS_COPY_SOURCE);
    CHECK(loweredTexture(list, 1).SyncAfter == D3D12_BARRIER_SYNC_NONE);
    CHECK(loweredTexture(list, 1).AccessAfter == D3D12_BARRIER_ACCESS_NO_ACCESS);
    // Every scope runs every access General admits; the Copy stage's other scopes run none of CopySrc's, so it waits on all.
    CHECK(loweredTexture(list, 2).SyncBefore == D3D12_BARRIER_SYNC_ALL);
    CHECK(loweredTexture(list, 2).AccessBefore == (D3D12_BARRIER_ACCESS_SHADER_RESOURCE | D3D12_BARRIER_ACCESS_COPY_SOURCE | D3D12_BARRIER_ACCESS_COPY_DEST));
    CHECK(loweredTexture(list, 2).SyncAfter == D3D12_BARRIER_SYNC_ALL);
    CHECK(loweredTexture(list, 2).AccessAfter == D3D12_BARRIER_ACCESS_COPY_SOURCE);
    submitAndWait(device, list, done, 2);
    const std::array<TextureBarrier, 3> leaving = { transition(textures[0], Layout::ShaderWrite, Layout::CopySrc, QueueType::Graphics),
                                                    transition(textures[1], Layout::ShaderRead, Layout::CopySrc, QueueType::Graphics),
                                                    transition(textures[2], Layout::CopySrc, Layout::CopyDst, QueueType::Graphics) };
    leave(device, leaving, done, 3);
    device.endFrame();
    CHECK(messages.count() == 0u);
    for (const Texture texture : textures)
    {
        device.destroy(texture);
    }
    device.destroy(buffer);
    device.destroy(done);
    destroyDevice(device);
}
