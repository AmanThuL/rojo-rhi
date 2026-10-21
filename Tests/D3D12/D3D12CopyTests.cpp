//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12CopyTests.cpp
/// @brief Tests D3D12 copies between buffers and textures: the native calls they record and the bytes they move.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "D3D12Addr.h"
#include "D3D12Common.h"
#include "D3D12Device.h"
#include "D3D12Textures.h"
#include "Support/D3D12/D3D12Readback.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace rojoRHI;

namespace {
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
    /// Returns a new device with the default frames in flight.
    Device makeDevice()
    {
        Result<Device> created = createDevice({});
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Returns a new buffer of `desc` on `device`, holding `initial` when it is not empty.
    Buffer makeBuffer(Device device, const BufferDesc& desc, std::span<const std::byte> initial = {})
    {
        Result<Buffer> created = device.createBuffer(desc, initial);
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
    /// Returns the native buffer of `buffer`, a live buffer of `device`.
    ID3D12Resource* resourceOf(Device device, Buffer buffer)
    {
        const auto* payload = static_cast<const d3d12::BufferPayload*>(DeviceImpl::of(device).core.pool<Buffer>().native(buffer));
        REQUIRE(payload != nullptr);
        return payload->resource;
    }

    //==================================================================================================================
    /// Returns the native texture of `texture`, a live texture of `device`.
    ID3D12Resource* resourceOf(Device device, Texture texture)
    {
        const auto* payload = static_cast<const d3d12::TexturePayload*>(DeviceImpl::of(device).core.pool<Texture>().native(texture));
        REQUIRE(payload != nullptr);
        return payload->resource;
    }

    //==================================================================================================================
    /// Returns the native calls `list` has recorded since its acquisition.
    uint64_t recordCalls(CommandList list)
    {
        return core::coreOf(list).counts.nativeRecordCalls;
    }

    //==================================================================================================================
    /// Returns `size` bytes counting up from `seed`, wrapping at 256.
    std::vector<std::byte> pattern(size_t size, uint32_t seed)
    {
        std::vector<std::byte> bytes(size);
        for (size_t i = 0; i < size; ++i)
        {
            bytes[i] = static_cast<std::byte>((i * 7 + seed) & 0xFF);
        }
        return bytes;
    }

    //==================================================================================================================
    /// Returns `images` images of `rows` rows of `rowBytes` bytes taken from `source`, rows `pitch` apart from `offset` and
    /// images `imageBytes` apart, joined into tight rows.
    std::vector<std::byte> gather(
        const std::vector<std::byte>& source, uint64_t offset, uint64_t pitch, uint64_t imageBytes, uint64_t images, uint64_t rows, uint64_t rowBytes
    )
    {
        std::vector<std::byte> tight;
        for (uint64_t image = 0; image < images; ++image)
        {
            for (uint64_t row = 0; row < rows; ++row)
            {
                const auto begin = source.begin() + static_cast<std::ptrdiff_t>(offset + image * imageBytes + row * pitch);
                tight.insert(tight.end(), begin, begin + static_cast<std::ptrdiff_t>(rowBytes));
            }
        }
        return tight;
    }

    /// The stage and access of one use of a texture subresource.
    struct Use
    {
        Stage stage = Stage::None;    ///< The stages the use runs in.
        Access access = Access::None; ///< What the use does to memory.
    };

    //==================================================================================================================
    /// Returns the use of a subresource in `layout` on a Graphics list.
    Use useOf(Layout layout)
    {
        switch (layout)
        {
        case Layout::General: return { Stage::Copy | Stage::ComputeShader, Access::CopyRead | Access::CopyWrite | Access::ShaderRead };
        case Layout::ShaderRead: return { Stage::FragmentShader | Stage::ComputeShader, Access::ShaderRead };
        case Layout::CopySrc: return { Stage::Copy, Access::CopyRead };
        case Layout::CopyDst: return { Stage::Copy, Access::CopyWrite };
        default: return {};
        }
    }

    //==================================================================================================================
    /// Returns a barrier that moves every subresource of `texture` from `from` to `to`.
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

    /// Submits lists one at a time in a frame, each signalling the next value of one fence and waited for on the host. A list of
    /// a Compute or Copy queue is joined into the frame by an empty Graphics submission that waits for its value.
    class Submitter
    {
      public:
        /// Opens a frame on `device`, which the submitter's fence belongs to.
        explicit Submitter(Device);

        /// Ends the frame; `device` is destroyed by the case.
        ~Submitter();

        //==============================================================================================================
        Submitter(const Submitter&) = delete;

        //==============================================================================================================
        Submitter& operator=(const Submitter&) = delete;

        /// Ends `list`, a list of `type`, submits it and waits until it has run.
        void run(CommandList, QueueType = QueueType::Graphics);

        /// Records the barrier of `texture` from `from` to `to` on a new Graphics list and runs it.
        void move(Texture, Layout, Layout);

      private:
        Device m_device;
        Fence m_fence;
        uint64_t m_value = 0;
    };

    //==================================================================================================================
    Submitter::Submitter(Device device) : m_device(device)
    {
        Result<Fence> created = device.createFence(0, "test.done");
        REQUIRE(created.has_value());
        m_fence = *created;
        device.beginFrame();
    }

    //==================================================================================================================
    Submitter::~Submitter()
    {
        m_device.endFrame();
        m_device.wait({ m_fence, m_value }, UINT64_MAX);
        m_device.destroy(m_fence);
    }

    //==================================================================================================================
    void Submitter::run(CommandList list, QueueType type)
    {
        list.end();
        ++m_value;
        const std::array<FenceValue, 1> signal{ FenceValue{ m_fence, m_value } };
        m_device.queue(type).submit(std::span(&list, 1), {}, signal);
        if (type != QueueType::Graphics)
        {
            m_device.queue(QueueType::Graphics).submit({}, signal, {});
        }
        REQUIRE(m_device.wait({ m_fence, m_value }, UINT64_MAX));
    }

    //==================================================================================================================
    void Submitter::move(Texture texture, Layout from, Layout to)
    {
        CommandList list = m_device.acquireCommandList(QueueType::Graphics, "test.move");
        const std::array<TextureBarrier, 1> barriers{ transition(texture, from, to) };
        list.barrier({}, {}, barriers);
        run(list);
    }
} // namespace

//======================================================================================================================
TEST_CASE("copyBuffer is one CopyBufferRegion on Graphics, Compute and Copy lists", "[d3d12]")
{
    const QueueType type = GENERATE(QueueType::Graphics, QueueType::Compute, QueueType::Copy);
    CAPTURE(static_cast<int>(type));
    constexpr uint64_t kSize = 256;
    Device device = makeDevice();
    const MessageCollection messages;
    const std::vector<std::byte> source = pattern(kSize, 3);
    const Buffer upload = makeBuffer(device, { .size = kSize, .memory = Memory::Upload, .label = "test.upload" }, source);
    const Buffer gpu = makeBuffer(device, { .size = kSize, .label = "test.gpu" });
    const Buffer readback = makeBuffer(device, { .size = kSize, .memory = Memory::Readback, .label = "test.readback" });
    {
        Submitter submitter(device);
        CommandList list = device.acquireCommandList(type, "test.copy");
        uint64_t before = recordCalls(list);
        list.copyBuffer(upload, 64, gpu, 128, 128);
        CHECK(recordCalls(list) - before == 1u);
        before = recordCalls(list);
        list.copyBuffer(upload, 0, readback, 0, kSize);
        CHECK(recordCalls(list) - before == 1u);
        submitter.run(list, type);
    }
    std::vector<std::byte> expected(kSize);
    std::memcpy(expected.data() + 128, source.data() + 64, 128);
    const std::vector<std::byte> read = test::readNativeBuffer(DeviceImpl::of(device), resourceOf(device, gpu), kSize);
    CHECK(std::memcmp(read.data() + 128, expected.data() + 128, 128) == 0);
    CHECK(std::memcmp(device.cpuPointer(readback), source.data(), kSize) == 0);
    CHECK(messages.count() == 0u);
    device.destroy(readback);
    device.destroy(gpu);
    device.destroy(upload);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a three-layer buffer-to-texture copy is three CopyTextureRegion and a 3D copy is one", "[d3d12]")
{
    const bool volume = GENERATE(false, true);
    CAPTURE(volume);
    constexpr uint32_t kImages = 3;
    constexpr uint64_t kPitch = 256;
    constexpr uint64_t kImageBytes = kPitch * 4;
    Device device = makeDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    const MessageCollection messages;
    const TextureDesc desc{ .type = volume ? TextureType::Tex3D : TextureType::Tex2DArray,
                            .format = Format::RGBA8Unorm,
                            .extent = { 4, 4, volume ? kImages : 1u },
                            .layers = volume ? 1u : kImages,
                            .usage = TextureUsage::Sampled,
                            .label = "test.texture" };
    const Texture texture = makeTexture(device, desc);
    const Texture copy = makeTexture(device, desc);
    const std::vector<std::byte> source = pattern(kImages * kImageBytes, 5);
    const Buffer upload = makeBuffer(device, { .size = source.size(), .memory = Memory::Upload, .label = "test.upload" }, source);
    const Buffer readback = makeBuffer(device, { .size = source.size(), .memory = Memory::Readback, .label = "test.readback" }, {});
    const BufferTextureLayout layout{ .offset = 0, .rowPitch = kPitch, .rowsPerImage = 4 };
    const TextureRegion region{ .layers = volume ? 1u : kImages, .extent = { 4, 4, volume ? kImages : 1u } };
    const uint64_t calls = volume ? 1 : kImages;
    {
        Submitter submitter(device);
        submitter.move(texture, Layout::Undefined, Layout::CopyDst);
        submitter.move(copy, Layout::Undefined, Layout::CopyDst);
        CommandList write = device.acquireCommandList(QueueType::Graphics, "test.write");
        uint64_t before = recordCalls(write);
        write.copyBufferToTexture(upload, layout, texture, region);
        CHECK(recordCalls(write) - before == calls);
        submitter.run(write);
        submitter.move(texture, Layout::CopyDst, Layout::CopySrc);
        CommandList read = device.acquireCommandList(QueueType::Graphics, "test.read");
        before = recordCalls(read);
        read.copyTextureToBuffer(texture, region, readback, layout);
        CHECK(recordCalls(read) - before == calls);
        before = recordCalls(read);
        read.copyTexture(texture, region, copy, region);
        CHECK(recordCalls(read) - before == calls);
        submitter.run(read);
    }
    const std::vector<std::byte> expected = gather(source, 0, kPitch, kImageBytes, kImages, 4, 16);
    // The copy back to the buffer moved the bytes the first copy wrote; the texture-to-texture copy moved them again.
    CHECK(
        gather(std::vector<std::byte>(device.cpuPointer(readback), device.cpuPointer(readback) + source.size()), 0, kPitch, kImageBytes, kImages, 4, 16) ==
        expected
    );
    std::vector<std::byte> copied;
    for (uint32_t image = 0; image < (volume ? 1u : kImages); ++image)
    {
        const std::vector<std::byte> part = test::readNativeTexture(impl, resourceOf(device, copy), image, D3D12_BARRIER_LAYOUT_COPY_DEST);
        copied.insert(copied.end(), part.begin(), part.end());
    }
    CHECK(copied == expected);
    CHECK(messages.count() == 0u);
    device.destroy(readback);
    device.destroy(upload);
    device.destroy(copy);
    device.destroy(texture);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a buffer-to-texture copy lands its bytes in the subresource it names", "[d3d12]")
{
    Device device = makeDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    const MessageCollection messages;
    SECTION("RGBA8, mip 1 of layer 2")
    {
        constexpr uint32_t kMips = 3;
        const TextureDesc desc{ .type = TextureType::Tex2DArray,
                                .format = Format::RGBA8Unorm,
                                .extent = { 8, 8, 1 },
                                .mips = kMips,
                                .layers = 3,
                                .usage = TextureUsage::Sampled,
                                .label = "test.texture" };
        const Texture texture = makeTexture(device, desc);
        const std::vector<std::byte> source = pattern(512 + 256 * 2, 9);
        const Buffer upload = makeBuffer(device, { .size = source.size(), .memory = Memory::Upload, .label = "test.upload" }, source);
        {
            Submitter submitter(device);
            submitter.move(texture, Layout::Undefined, Layout::CopyDst);
            CommandList list = device.acquireCommandList(QueueType::Graphics, "test.copy");
            list.copyBufferToTexture(
                upload, { .offset = 512, .rowPitch = 256 }, texture, { .mip = 1, .layer = 2, .offset = { 1, 1, 0 }, .extent = { 2, 2, 1 } }
            );
            submitter.run(list);
        }
        // Mip 1 is 4 texels wide; the region covers texels 1 and 2 of rows 1 and 2.
        const std::vector<std::byte> mip = test::readNativeTexture(impl, resourceOf(device, texture), 1 + 2 * kMips, D3D12_BARRIER_LAYOUT_COPY_DEST);
        REQUIRE(mip.size() == 4u * 4u * 4u);
        for (size_t row = 0; row < 2; ++row)
        {
            CHECK(std::memcmp(mip.data() + (row + 1) * 16 + 4, source.data() + 512 + row * 256, 8) == 0);
        }
        device.destroy(upload);
        device.destroy(texture);
    }
    SECTION("BC1, interior blocks and a block reaching the edge of mip 1")
    {
        if (!device.formatCapabilities(Format::BC1Unorm).sampled)
        {
            SKIP("the device does not sample BC1Unorm");
        }
        // 12 by 12 texels: 3 by 3 blocks at mip 0 and 6 by 6 texels, 2 by 2 blocks with the last ones partly outside, at mip 1.
        const TextureDesc desc{ .format = Format::BC1Unorm, .extent = { 12, 12, 1 }, .mips = 2, .usage = TextureUsage::Sampled, .label = "test.bc1" };
        const Texture texture = makeTexture(device, desc);
        const std::vector<std::byte> source = pattern(1024, 11);
        const Buffer upload = makeBuffer(device, { .size = source.size(), .memory = Memory::Upload, .label = "test.upload" }, source);
        {
            Submitter submitter(device);
            submitter.move(texture, Layout::Undefined, Layout::CopyDst);
            CommandList list = device.acquireCommandList(QueueType::Graphics, "test.copy");
            list.copyBufferToTexture(upload, { .offset = 0, .rowPitch = 256 }, texture, { .offset = { 4, 4, 0 }, .extent = { 8, 8, 1 } });
            list.copyBufferToTexture(upload, { .offset = 512, .rowPitch = 256 }, texture, { .mip = 1, .offset = { 4, 4, 0 }, .extent = { 2, 2, 1 } });
            submitter.run(list);
        }
        const std::vector<std::byte> mip0 = test::readNativeTexture(impl, resourceOf(device, texture), 0, D3D12_BARRIER_LAYOUT_COPY_DEST);
        REQUIRE(mip0.size() == 3u * 3u * 8u);
        for (size_t row = 0; row < 2; ++row)
        {
            CHECK(std::memcmp(mip0.data() + (row + 1) * 24 + 8, source.data() + row * 256, 16) == 0);
        }
        const std::vector<std::byte> mip1 = test::readNativeTexture(impl, resourceOf(device, texture), 1, D3D12_BARRIER_LAYOUT_COPY_DEST);
        REQUIRE(mip1.size() == 2u * 2u * 8u);
        CHECK(std::memcmp(mip1.data() + 16 + 8, source.data() + 512, 8) == 0);
        device.destroy(upload);
        device.destroy(texture);
    }
    CHECK(messages.count() == 0u);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("rowsPerImage zero and above the region's height place each layer where the layout says", "[d3d12]")
{
    // Zero takes the region's four rows; six leaves two rows of padding after each image. Both keep images 512-byte aligned.
    const uint32_t rowsPerImage = GENERATE(0u, 6u);
    CAPTURE(rowsPerImage);
    constexpr uint64_t kPitch = 256;
    const uint64_t imageBytes = kPitch * (rowsPerImage != 0 ? rowsPerImage : 4);
    Device device = makeDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    const MessageCollection messages;
    const TextureDesc desc{ .type = TextureType::Tex2DArray,
                            .format = Format::RGBA8Unorm,
                            .extent = { 4, 4, 1 },
                            .layers = 2,
                            .usage = TextureUsage::Sampled,
                            .label = "test.texture" };
    const Texture texture = makeTexture(device, desc);
    // The images start 512 bytes in.
    const std::vector<std::byte> source = pattern(512 + 2 * imageBytes, 13);
    const Buffer upload = makeBuffer(device, { .size = source.size(), .memory = Memory::Upload, .label = "test.upload" }, source);
    {
        Submitter submitter(device);
        submitter.move(texture, Layout::Undefined, Layout::CopyDst);
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.copy");
        list.copyBufferToTexture(upload, { .offset = 512, .rowPitch = kPitch, .rowsPerImage = rowsPerImage }, texture, { .layers = 2, .extent = { 4, 4, 1 } });
        submitter.run(list);
    }
    for (uint32_t layer = 0; layer < 2; ++layer)
    {
        CAPTURE(layer);
        CHECK(
            test::readNativeTexture(impl, resourceOf(device, texture), layer, D3D12_BARRIER_LAYOUT_COPY_DEST) ==
            gather(source, 512 + layer * imageBytes, kPitch, 0, 1, 4, 16)
        );
    }
    CHECK(messages.count() == 0u);
    device.destroy(upload);
    device.destroy(texture);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a D32Float texture copies to a buffer and back", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const TextureDesc desc{
        .format = Format::D32Float, .extent = { 4, 4, 1 }, .usage = TextureUsage::DepthStencil | TextureUsage::Sampled, .label = "test.depth"
    };
    const Texture texture = makeTexture(device, desc);
    const std::vector<std::byte> source = pattern(256 * 4, 17);
    const Buffer upload = makeBuffer(device, { .size = source.size(), .memory = Memory::Upload, .label = "test.upload" }, source);
    const Buffer readback = makeBuffer(device, { .size = source.size(), .memory = Memory::Readback, .label = "test.readback" });
    const BufferTextureLayout layout{ .offset = 0, .rowPitch = 256 };
    const TextureRegion region{ .extent = { 4, 4, 1 } };
    {
        Submitter submitter(device);
        submitter.move(texture, Layout::Undefined, Layout::CopyDst);
        CommandList write = device.acquireCommandList(QueueType::Graphics, "test.write");
        write.copyBufferToTexture(upload, layout, texture, region);
        submitter.run(write);
        submitter.move(texture, Layout::CopyDst, Layout::CopySrc);
        CommandList read = device.acquireCommandList(QueueType::Graphics, "test.read");
        read.copyTextureToBuffer(texture, region, readback, layout);
        submitter.run(read);
    }
    const std::vector<std::byte> expected = gather(source, 0, 256, 0, 1, 4, 16);
    CHECK(gather(std::vector<std::byte>(device.cpuPointer(readback), device.cpuPointer(readback) + source.size()), 0, 256, 0, 1, 4, 16) == expected);
    CHECK(messages.count() == 0u);
    device.destroy(readback);
    device.destroy(upload);
    device.destroy(texture);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("copyTexture between an sRGB format and its linear partner moves the stored bits", "[d3d12]")
{
    Device device = makeDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    const MessageCollection messages;
    const auto desc = [](Format format, std::string_view label) {
        return TextureDesc{ .format = format, .extent = { 8, 8, 1 }, .usage = TextureUsage::Sampled, .label = label };
    };
    const Texture srgb = makeTexture(device, desc(Format::RGBA8Unorm_sRGB, "test.srgb"));
    const Texture linear = makeTexture(device, desc(Format::RGBA8Unorm, "test.linear"));
    const std::vector<std::byte> source = pattern(256 * 8, 19);
    const Buffer upload = makeBuffer(device, { .size = source.size(), .memory = Memory::Upload, .label = "test.upload" }, source);
    const TextureRegion whole{ .extent = { 8, 8, 1 } };
    {
        Submitter submitter(device);
        submitter.move(srgb, Layout::Undefined, Layout::CopyDst);
        submitter.move(linear, Layout::Undefined, Layout::CopyDst);
        CommandList write = device.acquireCommandList(QueueType::Graphics, "test.write");
        write.copyBufferToTexture(upload, { .offset = 0, .rowPitch = 256 }, srgb, whole);
        submitter.run(write);
        submitter.move(srgb, Layout::CopyDst, Layout::CopySrc);
        CommandList copy = device.acquireCommandList(QueueType::Graphics, "test.copy");
        copy.copyTexture(srgb, whole, linear, whole);
        submitter.run(copy);
    }
    CHECK(test::readNativeTexture(impl, resourceOf(device, linear), 0, D3D12_BARRIER_LAYOUT_COPY_DEST) == gather(source, 0, 256, 0, 1, 8, 32));
    CHECK(messages.count() == 0u);
    device.destroy(upload);
    device.destroy(linear);
    device.destroy(srgb);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a copy on a Copy list from a General texture submits clean", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const std::vector<std::byte> bytes = pattern(4 * 16, 23);
    const TextureData data{ bytes, 16, 0 };
    const Texture texture = makeTexture(
        device, { .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled, .label = "test.texture" }, std::span(&data, 1)
    );
    const Buffer readback = makeBuffer(device, { .size = 256 * 4, .memory = Memory::Readback, .label = "test.readback" });
    {
        Submitter submitter(device);
        // A texture created with initial data starts in ShaderRead, which a Copy list cannot use.
        submitter.move(texture, Layout::ShaderRead, Layout::General);
        CommandList list = device.acquireCommandList(QueueType::Copy, "test.copy");
        const uint64_t before = recordCalls(list);
        list.copyTextureToBuffer(texture, { .extent = { 4, 4, 1 } }, readback, { .offset = 0, .rowPitch = 256 });
        CHECK(recordCalls(list) - before == 1u);
        submitter.run(list, QueueType::Copy);
    }
    std::vector<std::byte> read(device.cpuPointer(readback), device.cpuPointer(readback) + 256 * 4);
    CHECK(gather(read, 0, 256, 0, 1, 4, 16) == bytes);
    CHECK(messages.count() == 0u);
    device.destroy(readback);
    device.destroy(texture);
    destroyDevice(device);
}
