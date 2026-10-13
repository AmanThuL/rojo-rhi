//----------------------------------------------------------------------------------------------------------------------
/// @file NullSparseTests.cpp
/// @brief Tests the tile shapes the Null backend reports.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    TileShape shapeOf(TextureType type, Format format, Extent3D extent, uint32_t mips = 1, uint32_t layers = 1)
    {
        Device device = *createDevice();
        const Texture texture = *device.createTexture(
            TextureDesc{ .type = type, .format = format, .extent = extent, .mips = mips, .layers = layers, .usage = TextureUsage::Sparse }
        );
        const TileShape shape = device.tileShape(texture);
        destroyDevice(device);
        return shape;
    }
} // namespace

//======================================================================================================================
TEST_CASE("a Null tile is 64 KiB for every format and kind", "[null]")
{
    CHECK(shapeOf(TextureType::Tex2D, Format::R8Unorm, { 512, 512, 1 }).bytes == 65536);
    CHECK(shapeOf(TextureType::Tex3D, Format::RGBA16Float, { 64, 64, 64 }).bytes == 65536);
    CHECK(shapeOf(TextureType::Tex2D, Format::BC1Unorm, { 1024, 1024, 1 }).bytes == 65536);
}

//======================================================================================================================
TEST_CASE("Null 2D tile extents follow the D3D12 standard shapes", "[null]")
{
    const TileShape r8 = shapeOf(TextureType::Tex2D, Format::R8Unorm, { 512, 512, 1 });
    CHECK((r8.width == 256 && r8.height == 256 && r8.depth == 1));
    const TileShape r16 = shapeOf(TextureType::Tex2D, Format::R16Float, { 512, 512, 1 });
    CHECK((r16.width == 256 && r16.height == 128 && r16.depth == 1));
    const TileShape r32 = shapeOf(TextureType::Tex2D, Format::RGBA8Unorm, { 512, 512, 1 });
    CHECK((r32.width == 128 && r32.height == 128 && r32.depth == 1));
    const TileShape r64 = shapeOf(TextureType::Tex2D, Format::RGBA16Float, { 512, 512, 1 });
    CHECK((r64.width == 128 && r64.height == 64 && r64.depth == 1));
}

//======================================================================================================================
TEST_CASE("Null 3D tile extents follow the D3D12 standard shapes", "[null]")
{
    const TileShape r8 = shapeOf(TextureType::Tex3D, Format::R8Unorm, { 64, 64, 64 });
    CHECK((r8.width == 64 && r8.height == 32 && r8.depth == 32));
    const TileShape r32 = shapeOf(TextureType::Tex3D, Format::RGBA8Unorm, { 64, 64, 64 });
    CHECK((r32.width == 32 && r32.height == 32 && r32.depth == 16));
    const TileShape r64 = shapeOf(TextureType::Tex3D, Format::RGBA16Float, { 64, 64, 64 });
    CHECK((r64.width == 32 && r64.height == 16 && r64.depth == 16));
}

//======================================================================================================================
TEST_CASE("a block-compressed Null tile covers its block shape counted in texels", "[null]")
{
    const TileShape bc1 = shapeOf(TextureType::Tex2D, Format::BC1Unorm, { 1024, 1024, 1 });
    CHECK((bc1.width == 512 && bc1.height == 256 && bc1.depth == 1));
}

//======================================================================================================================
TEST_CASE("the Null mip tail starts at the first level smaller than a tile", "[null]")
{
    const TileShape shape = shapeOf(TextureType::Tex2D, Format::RGBA8Unorm, { 512, 512, 1 }, 10);
    CHECK(shape.mipTailFirst == 3);
    CHECK(shape.mipTailBytes == 65536);
}

//======================================================================================================================
TEST_CASE("one narrow axis puts a level in the Null mip tail", "[null]")
{
    const TileShape shape = shapeOf(TextureType::Tex2D, Format::RGBA8Unorm, { 512, 128, 1 }, 8);
    CHECK(shape.mipTailFirst == 1);
}

//======================================================================================================================
TEST_CASE("a 3D Null texture's tail also counts depth", "[null]")
{
    const TileShape shape = shapeOf(TextureType::Tex3D, Format::RGBA8Unorm, { 64, 64, 64 }, 7);
    CHECK(shape.mipTailFirst == 2);
}

//======================================================================================================================
TEST_CASE("a Null sparse buffer's tile is 64 KiB wide", "[null]")
{
    Device device = *createDevice();
    const Buffer buffer = *device.createBuffer(BufferDesc{ .size = 4 * 65536, .memory = Memory::Gpu, .usage = BufferUsage::Sparse });
    const TileShape shape = device.tileShape(buffer);
    CHECK(shape.width == 65536);
    CHECK(shape.bytes == 65536);
    destroyDevice(device);
}
