//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12StagerTests.cpp
/// @brief Tests the D3D12 stager: staged buffer and texture data is in place when an upload returns, and uploads serialize.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "D3D12Common.h"
#include "D3D12Device.h"
#include "D3D12Stager.h"
#include "Support/D3D12/D3D12Readback.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace rojoRHI;

namespace {
    /// The rows and depth slices of one subresource's staged bytes.
    struct Subresource
    {
        std::vector<std::byte> bytes; ///< The staged bytes, rows `bytesPerRow` apart and slices `bytesPerImage` apart.
        uint64_t bytesPerRow = 0;     ///< The distance between rows of `bytes`.
        uint64_t bytesPerImage = 0;   ///< The distance between depth slices of `bytes`; 0 outside a 3D texture.
        std::vector<std::byte> tight; ///< The same texels in tight rows and slices, as `readNativeTexture` returns them.
    };

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

    /// Collects the library's warnings and errors into `g_messages`, emptied first, while it lives, and removes the callback
    /// when it goes, however the case ends.
    class MessageCollection
    {
      public:
        MessageCollection();
        ~MessageCollection();

        //==============================================================================================================
        MessageCollection(const MessageCollection&) = delete;

        //==============================================================================================================
        MessageCollection& operator=(const MessageCollection&) = delete;
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
    /// Returns `size` bytes counting up from `seed`, wrapping at 256.
    std::vector<std::byte> pattern(size_t size, uint32_t seed)
    {
        std::vector<std::byte> bytes(size);
        for (size_t i = 0; i < size; ++i)
        {
            bytes[i] = static_cast<std::byte>((i + seed) & 0xFF);
        }
        return bytes;
    }

    //==================================================================================================================
    /// Returns a subresource of `slices` slices of `rows` rows of `rowSize` bytes, staged with rows `bytesPerRow` apart and,
    /// when `bytesPerImage` is not 0, slices `bytesPerImage` apart. Every staged byte, padding included, comes from `seed`.
    Subresource stagedRows(uint64_t rowSize, uint64_t rows, uint64_t slices, uint64_t bytesPerRow, uint64_t bytesPerImage, uint32_t seed)
    {
        Subresource result;
        result.bytesPerRow = bytesPerRow;
        result.bytesPerImage = bytesPerImage;
        const uint64_t sliceStride = bytesPerImage != 0 ? bytesPerImage : bytesPerRow * rows;
        // The last row is not padded to the stride.
        result.bytes = pattern(sliceStride * (slices - 1) + bytesPerRow * (rows - 1) + rowSize, seed);
        for (uint64_t slice = 0; slice < slices; ++slice)
        {
            for (uint64_t row = 0; row < rows; ++row)
            {
                const std::byte* start = result.bytes.data() + slice * sliceStride + row * bytesPerRow;
                result.tight.insert(result.tight.end(), start, start + rowSize);
            }
        }
        return result;
    }

    //==================================================================================================================
    /// Returns the stager's view of `staged`.
    d3d12::SubresourceData dataOf(const Subresource& staged)
    {
        return d3d12::SubresourceData{ .bytes = staged.bytes, .bytesPerRow = staged.bytesPerRow, .bytesPerImage = staged.bytesPerImage };
    }

    //==================================================================================================================
    /// Returns a new committed `DEFAULT` buffer of `size` bytes on `impl`'s device, labelled `test.destination`.
    d3d12::Com<ID3D12Resource> createBuffer(DeviceImpl& impl, uint64_t size)
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC1 desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        d3d12::Com<ID3D12Resource> buffer;
        REQUIRE(SUCCEEDED(impl.device()->CreateCommittedResource3(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(buffer.put())
        )));
        d3d12::setName(buffer.get(), "test.destination");
        return buffer;
    }

    //==================================================================================================================
    /// Returns a new committed `DEFAULT` texture of `dimension` and `format` on `impl`'s device, `depthOrLayers` deep or
    /// layered and with `mips` mips, created in `D3D12_BARRIER_LAYOUT_COPY_DEST` and labelled `test.destination`.
    d3d12::Com<ID3D12Resource> createTexture(
        DeviceImpl& impl, D3D12_RESOURCE_DIMENSION dimension, DXGI_FORMAT format, uint32_t width, uint32_t height, uint16_t depthOrLayers, uint16_t mips
    )
    {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC1 desc = {};
        desc.Dimension = dimension;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = depthOrLayers;
        desc.MipLevels = mips;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d3d12::Com<ID3D12Resource> texture;
        REQUIRE(SUCCEEDED(impl.device()->CreateCommittedResource3(
            &heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_COPY_DEST, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(texture.put())
        )));
        d3d12::setName(texture.get(), "test.destination");
        return texture;
    }
} // namespace

//======================================================================================================================
TEST_CASE("staged buffer data is in the buffer when uploadBuffer returns", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    {
        d3d12::Com<ID3D12Resource> buffer = createBuffer(impl, 4096);
        const std::vector<std::byte> bytes = pattern(4096, 0);
        const Result<void> uploaded = impl.stager.uploadBuffer(impl, buffer.get(), bytes);
        INFO((uploaded.has_value() ? std::string() : uploaded.error().message));
        REQUIRE(uploaded.has_value());
        CHECK(test::readNativeBuffer(impl, buffer.get(), 4096) == bytes);
    }
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("staged data shorter than the buffer fills only its prefix", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    {
        d3d12::Com<ID3D12Resource> buffer = createBuffer(impl, 256);
        const std::vector<std::byte> bytes = pattern(100, 7);
        REQUIRE(impl.stager.uploadBuffer(impl, buffer.get(), bytes).has_value());
        const std::vector<std::byte> read = test::readNativeBuffer(impl, buffer.get(), 256);
        REQUIRE(read.size() == 256);
        CHECK(std::memcmp(read.data(), bytes.data(), bytes.size()) == 0);
    }
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("every mip and layer of a texture reads back its staged rows", "[d3d12]")
{
    constexpr uint32_t kMips = 3;
    constexpr uint32_t kLayers = 2;
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    {
        d3d12::Com<ID3D12Resource> texture = createTexture(impl, D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM, 16, 8, kLayers, kMips);
        // D3D12 subresource order: index mip + layer * mips. Tight rows of 64, 32 and 16 bytes, none a multiple of 256.
        std::vector<Subresource> subresources;
        for (uint32_t layer = 0; layer < kLayers; ++layer)
        {
            for (uint32_t mip = 0; mip < kMips; ++mip)
            {
                const uint64_t rowSize = uint64_t{ 16u >> mip } * 4;
                subresources.push_back(stagedRows(rowSize, 8u >> mip, 1, rowSize, 0, mip + layer * kMips + 1));
            }
        }
        std::vector<d3d12::SubresourceData> data;
        for (const Subresource& each : subresources)
        {
            data.push_back(dataOf(each));
        }
        const Result<void> uploaded = impl.stager.uploadTexture(impl, texture.get(), data);
        INFO((uploaded.has_value() ? std::string() : uploaded.error().message));
        REQUIRE(uploaded.has_value());
        for (uint32_t index = 0; index < kMips * kLayers; ++index)
        {
            INFO("subresource " << index);
            CHECK(test::readNativeTexture(impl, texture.get(), index, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE) == subresources[index].tight);
        }
    }
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("a volume's slices read back their staged rows", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    {
        d3d12::Com<ID3D12Resource> texture = createTexture(impl, D3D12_RESOURCE_DIMENSION_TEXTURE3D, DXGI_FORMAT_R8G8B8A8_UNORM, 8, 4, 3, 1);
        // Rows of 32 bytes, tight; slices 192 bytes apart, 64 bytes past the 128 a slice's rows take.
        const Subresource volume = stagedRows(32, 4, 3, 32, 192, 11);
        const std::array data{ dataOf(volume) };
        REQUIRE(impl.stager.uploadTexture(impl, texture.get(), data).has_value());
        CHECK(test::readNativeTexture(impl, texture.get(), 0, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE) == volume.tight);
    }
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("a BC1 texture's block rows read back", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    {
        d3d12::Com<ID3D12Resource> texture = createTexture(impl, D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_BC1_UNORM, 8, 8, 1, 1);
        // Two rows of two 8-byte blocks.
        const Subresource blocks = stagedRows(16, 2, 1, 16, 0, 23);
        const std::array data{ dataOf(blocks) };
        REQUIRE(impl.stager.uploadTexture(impl, texture.get(), data).has_value());
        CHECK(test::readNativeTexture(impl, texture.get(), 0, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE) == blocks.tight);
    }
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("an empty subresource is skipped and the texture still leaves COPY_DEST", "[d3d12]")
{
    const MessageCollection collecting;
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    {
        d3d12::Com<ID3D12Resource> texture = createTexture(impl, D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 2);
        const Subresource top = stagedRows(32, 8, 1, 32, 0, 31);
        const std::array data{ dataOf(top), d3d12::SubresourceData{} };
        REQUIRE(impl.stager.uploadTexture(impl, texture.get(), data).has_value());
        CHECK(test::readNativeTexture(impl, texture.get(), 0, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE) == top.tight);
        // Mip 1's contents are undefined; reading it from SHADER_RESOURCE shows the barrier covered it too, since a layout
        // mismatch there is a debug-layer message.
        CHECK(test::readNativeTexture(impl, texture.get(), 1, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE).size() == 4 * 4 * 4);
    }
    destroyDevice(*created);
    const std::lock_guard lock(g_messagesMutex);
    std::string reported;
    for (const std::string& message : g_messages)
    {
        reported += message + "\n";
    }
    INFO(reported);
    CHECK(g_messages.empty());
}

//======================================================================================================================
TEST_CASE("a texture whose entries are all empty leaves COPY_DEST without a staging buffer or a debug-layer message", "[d3d12]")
{
    const MessageCollection collecting;
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    {
        d3d12::Com<ID3D12Resource> texture = createTexture(impl, D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8B8A8_UNORM, 8, 8, 1, 2);
        const std::array data{ d3d12::SubresourceData{}, d3d12::SubresourceData{} };
        const Counters before = created->counters();
        REQUIRE(impl.stager.uploadTexture(impl, texture.get(), data).has_value());
        CHECK(created->counters().nativeCreateCalls == before.nativeCreateCalls);
        // Both mips' contents are undefined; reading them from SHADER_RESOURCE shows the barrier moved them there.
        CHECK(test::readNativeTexture(impl, texture.get(), 0, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE).size() == 8 * 8 * 4);
        CHECK(test::readNativeTexture(impl, texture.get(), 1, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE).size() == 4 * 4 * 4);
    }
    destroyDevice(*created);
    const std::lock_guard lock(g_messagesMutex);
    std::string reported;
    for (const std::string& message : g_messages)
    {
        reported += message + "\n";
    }
    INFO(reported);
    CHECK(g_messages.empty());
}

//======================================================================================================================
TEST_CASE("a staged upload counts one staging buffer, two queue calls and its host calls", "[d3d12]")
{
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    {
        d3d12::Com<ID3D12Resource> buffer = createBuffer(impl, 256);
        const std::vector<std::byte> bytes = pattern(256, 3);
        const Counters before = created->counters();
        REQUIRE(impl.stager.uploadBuffer(impl, buffer.get(), bytes).has_value());
        const Counters after = created->counters();
        CHECK(after.nativeCreateCalls == before.nativeCreateCalls + 1);
        CHECK(after.nativeSubmitCalls == before.nativeSubmitCalls + 2);
        // The Map of the staging buffer and at least the wait's first read of the fence.
        CHECK(after.nativeHostCalls >= before.nativeHostCalls + 2);
    }
    destroyDevice(*created);
}

//======================================================================================================================
TEST_CASE("uploads from two threads are serialized and both complete", "[d3d12]")
{
    constexpr size_t kSize = 64 * 1024;
    constexpr uint32_t kRounds = 50;
    Result<Device> created = createDevice();
    REQUIRE(created.has_value());
    DeviceImpl& impl = DeviceImpl::of(*created);
    {
        const std::array buffers{ createBuffer(impl, kSize), createBuffer(impl, kSize) };
        // Thread t uploads the pattern seeded 2 * round + t in each round, so the two threads' patterns always differ and the
        // last round's is the one each buffer keeps.
        std::atomic<uint32_t> failures = 0;
        {
            std::array<std::jthread, 2> threads;
            for (uint32_t index = 0; index < threads.size(); ++index)
            {
                threads[index] = std::jthread([&impl, &buffers, &failures, index] {
                    for (uint32_t round = 0; round < kRounds; ++round)
                    {
                        const std::vector<std::byte> bytes = pattern(kSize, 2 * round + index);
                        if (!impl.stager.uploadBuffer(impl, buffers[index].get(), bytes).has_value())
                        {
                            failures.fetch_add(1);
                        }
                    }
                });
            }
        }
        CHECK(failures.load() == 0);
        CHECK(test::readNativeBuffer(impl, buffers[0].get(), kSize) == pattern(kSize, 2 * (kRounds - 1)));
        CHECK(test::readNativeBuffer(impl, buffers[1].get(), kSize) == pattern(kSize, 2 * (kRounds - 1) + 1));
    }
    destroyDevice(*created);
}
