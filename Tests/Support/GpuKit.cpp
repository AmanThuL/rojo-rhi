//----------------------------------------------------------------------------------------------------------------------
/// @file GpuKit.cpp
/// @brief Implements the helper that runs recorded lists on a GPU and reads buffers and textures back for the conformance cases.
//----------------------------------------------------------------------------------------------------------------------

#include "Support/GpuKit.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rojoRHI::test {
    namespace {
        /// How one format packs texels, read from chapter 15's table: the block edge in texels and the bytes of one block.
        struct Packing
        {
            uint32_t blockEdge = 1;  ///< Texels of one block along x and y.
            uint32_t blockBytes = 4; ///< Bytes of one block.
        };

        //==============================================================================================================
        /// Returns the packing of `format`; fails the case for a format without a packed texel or block size.
        Packing packingOf(Format format)
        {
            switch (format)
            {
            case Format::BGRA8Unorm:
            case Format::RGBA8Unorm:
            case Format::RGBA8Unorm_sRGB:
            case Format::RG16Float:
            case Format::D32Float:
            case Format::R32Float:
            case Format::RGB10A2Unorm: return { 1, 4 };
            case Format::RGBA16Float: return { 1, 8 };
            case Format::R8Unorm: return { 1, 1 };
            case Format::R16Float: return { 1, 2 };
            case Format::BC1Unorm:
            case Format::BC1Unorm_sRGB: return { 4, 8 };
            case Format::Undefined:
            case Format::D32FloatS8: break;
            }
            // Undefined names no texels, and the reference gives D32FloatS8 no packed size; neither is copied (VU-rojo-cmd-0041).
            FAIL_CHECK("GpuKit reads no texels of a format without a packed size");
            return {};
        }

        //==============================================================================================================
        /// Rounds `value` up to a multiple of `alignment`.
        uint64_t alignUp(uint64_t value, uint64_t alignment)
        {
            return (value + alignment - 1) / alignment * alignment;
        }

        //==============================================================================================================
        /// A list handle has no comparison of its own; two handles name one list when their bits are equal.
        bool sameList(CommandList a, CommandList b)
        {
            return std::memcmp(&a, &b, sizeof(CommandList)) == 0;
        }
    } // namespace

    //==================================================================================================================
    GpuKit::GpuKit(uint32_t framesInFlight)
    {
        device = *createDevice(DeviceDesc{ .framesInFlight = framesInFlight, .label = "kit.device" });
        m_fence = *device.createFence(0, "kit.fence");
        device.beginFrame();
    }

    //==================================================================================================================
    GpuKit::~GpuKit()
    {
        device.endFrame();
        destroyDevice(device);
    }

    //==================================================================================================================
    CommandList GpuKit::list(QueueType type, std::string_view label)
    {
        const CommandList list = device.acquireCommandList(type, label);
        m_lists.push_back(Acquired{ list, type });
        return list;
    }

    //==================================================================================================================
    void GpuKit::run(std::span<const CommandList> lists, std::span<const FenceValue> wait)
    {
        for (CommandList list : lists)
        {
            list.end();
        }
        std::array<FenceValue, 1> previous{};
        std::span<const FenceValue> waits = wait;
        bool joined = true;
        size_t first = 0;
        while (first < lists.size())
        {
            const QueueType type = typeOf(lists[first]);
            size_t last = first + 1;
            while (last < lists.size() && typeOf(lists[last]) == type)
            {
                ++last;
            }
            const std::array<FenceValue, 1> signal{ FenceValue{ m_fence, ++m_value } };
            device.queue(type).submit(lists.subspan(first, last - first), waits, signal);
            previous = signal;
            waits = previous;
            joined = type == QueueType::Graphics;
            first = last;
        }
        if (!joined)
        {
            // The last submission went to a compute or copy queue; a graphics submission waiting on its value joins it, and
            // every submission before it through the chain of waits.
            device.queue(QueueType::Graphics).submit({}, previous);
        }
        if (m_value > 0)
        {
            REQUIRE(device.wait(FenceValue{ m_fence, m_value }, UINT64_MAX));
        }
    }

    //==================================================================================================================
    Buffer GpuKit::buffer(uint64_t size, Memory memory, BufferUsage usage, std::span<const std::byte> initial, Placement placement)
    {
        const Buffer buffer = *device.createBuffer(BufferDesc{ .size = size, .memory = memory, .usage = usage, .label = "kit.buffer" }, initial, placement);
        m_buffers.push_back(Sized{ buffer, size });
        return buffer;
    }

    //==================================================================================================================
    std::vector<std::byte> GpuKit::read(Buffer readback)
    {
        const uint64_t size = sizeOf(readback);
        const std::byte* pointer = device.cpuPointer(readback);
        REQUIRE(pointer != nullptr);
        return std::vector<std::byte>(pointer, pointer + size);
    }

    //==================================================================================================================
    std::vector<std::byte> GpuKit::readBuffer(Buffer source, QueueType type)
    {
        const uint64_t size = sizeOf(source);
        const Buffer readback = buffer(size, Memory::Readback);
        CommandList copy = list(type, "kit.readBuffer");
        copy.copyBuffer(source, 0, readback, 0, size);
        run(std::array{ copy });
        std::vector<std::byte> bytes = read(readback);
        return bytes;
    }

    //==================================================================================================================
    std::vector<std::byte> GpuKit::readTexture(Texture texture, const TextureRegion& region, Format format)
    {
        const Packing packing = packingOf(format);
        const uint64_t blocksWide = (region.extent.width + packing.blockEdge - 1) / packing.blockEdge;
        const uint64_t rows = (region.extent.height + packing.blockEdge - 1) / packing.blockEdge;
        const uint64_t tightRow = blocksWide * packing.blockBytes;
        const uint64_t rowPitch = alignUp(tightRow, 256);
        const uint64_t images = uint64_t{ region.extent.depth } * region.layers;
        // Consecutive images sit a multiple of 512 bytes apart (VU-rojo-cmd-0037), so an odd number of 256-byte rows takes one
        // more row of padding.
        uint64_t rowsPerImage = rows;
        if (images > 1 && rowPitch * rowsPerImage % 512 != 0)
        {
            ++rowsPerImage;
        }
        const uint64_t imageBytes = rowPitch * rowsPerImage;
        const Buffer readback = buffer(imageBytes * images, Memory::Readback);
        CommandList copy = list(QueueType::Graphics, "kit.readTexture");
        const BufferTextureLayout layout{ .offset = 0, .rowPitch = static_cast<uint32_t>(rowPitch), .rowsPerImage = static_cast<uint32_t>(rowsPerImage) };
        copy.copyTextureToBuffer(texture, region, readback, layout);
        run(std::array{ copy });
        const std::vector<std::byte> padded = read(readback);
        std::vector<std::byte> tight(tightRow * rows * images);
        for (uint64_t image = 0; image < images; ++image)
        {
            for (uint64_t row = 0; row < rows; ++row)
            {
                std::memcpy(tight.data() + (image * rows + row) * tightRow, padded.data() + image * imageBytes + row * rowPitch, tightRow);
            }
        }
        return tight;
    }

    //==================================================================================================================
    QueueType GpuKit::typeOf(CommandList list) const
    {
        const auto found = std::ranges::find_if(m_lists, [&](const Acquired& acquired) { return sameList(acquired.list, list); });
        if (found == m_lists.end())
        {
            FAIL("GpuKit::run was given a list the kit did not acquire");
        }
        return found->type;
    }

    //==================================================================================================================
    uint64_t GpuKit::sizeOf(Buffer buffer) const
    {
        const auto found = std::ranges::find_if(m_buffers, [&](const Sized& sized) { return sized.buffer.bits == buffer.bits; });
        if (found == m_buffers.end())
        {
            FAIL("GpuKit was given a buffer the kit did not create");
        }
        return found->size;
    }

    //==================================================================================================================
    std::vector<std::byte> pattern(size_t count, uint32_t seed)
    {
        std::vector<std::byte> bytes(count);
        for (size_t i = 0; i < count; ++i)
        {
            bytes[i] = static_cast<std::byte>((seed * 31 + i * 7) % 251 + 1);
        }
        return bytes;
    }
} // namespace rojoRHI::test
