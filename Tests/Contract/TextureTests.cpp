//----------------------------------------------------------------------------------------------------------------------
/// @file TextureTests.cpp
/// @brief Tests texture and view creation, initial data, placement and view slots against chapter 04.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <set>
#include <vector>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    Device makeDevice()
    {
        Result<Device> created = createDevice();
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    Texture makeTexture(Device device, const TextureDesc& desc, std::span<const TextureData> initial = {}, Placement placement = {})
    {
        Result<Texture> created = device.createTexture(desc, initial, placement);
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    ViewIndex makeView(Device device, Texture texture, const TextureViewDesc& desc = {})
    {
        Result<ViewIndex> created = device.createView(texture, desc);
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    bool isPowerOfTwo(uint64_t value)
    {
        return value != 0 && (value & (value - 1)) == 0;
    }

} // namespace

//======================================================================================================================
TEST_CASE("a texture is created in every type", "[contract]")
{
    Device device = makeDevice();
    struct Case
    {
        TextureType type;
        Extent3D extent;
        uint32_t layers;
    };
    const std::array<Case, 7> cases = { {
        { TextureType::Tex1D, { 16, 1, 1 }, 1 },
        { TextureType::Tex1DArray, { 16, 1, 1 }, 3 },
        { TextureType::Tex2D, { 16, 8, 1 }, 1 },
        { TextureType::Tex2DArray, { 16, 8, 1 }, 3 },
        { TextureType::Tex3D, { 8, 8, 8 }, 1 },
        { TextureType::Cube, { 8, 8, 1 }, 6 },
        { TextureType::CubeArray, { 8, 8, 1 }, 12 },
    } };
    for (const Case& item : cases)
    {
        const Texture texture = makeTexture(
            device, { .type = item.type, .format = Format::RGBA8Unorm, .extent = item.extent, .layers = item.layers, .usage = TextureUsage::Sampled }
        );
        CHECK(texture != Texture{});
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("two textures have different handles", "[contract]")
{
    Device device = makeDevice();
    const TextureDesc desc{ .format = Format::R8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled };
    CHECK(makeTexture(device, desc) != makeTexture(device, desc));
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a texture takes a full mip chain and every attachment usage", "[contract]")
{
    Device device = makeDevice();
    CHECK(device.createTexture({ .format = Format::RGBA8Unorm, .extent = { 64, 64, 1 }, .mips = 7, .usage = TextureUsage::Sampled | TextureUsage::Storage })
              .has_value());
    CHECK(device
              .createTexture(
                  { .format = Format::RGBA16Float, .extent = { 64, 64, 1 }, .samples = 4, .usage = TextureUsage::RenderTarget | TextureUsage::Sampled }
              )
              .has_value());
    CHECK(
        device.createTexture({ .format = Format::D32FloatS8, .extent = { 64, 64, 1 }, .usage = TextureUsage::DepthStencil | TextureUsage::Sampled }).has_value()
    );
    CHECK(device.createTexture({ .format = Format::BC1Unorm, .extent = { 64, 64, 1 }, .mips = 7, .usage = TextureUsage::Sampled }).has_value());
    if (device.capabilities().placementSparse)
    {
        CHECK(
            device.createTexture({ .format = Format::RGBA8Unorm, .extent = { 64, 64, 1 }, .usage = TextureUsage::Sampled | TextureUsage::Sparse }).has_value()
        );
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a texture takes initial data for every subresource", "[contract]")
{
    Device device = makeDevice();
    const std::vector<std::byte> level0(8 * 8 * 4), level1(4 * 4 * 4);
    const std::array<TextureData, 4> data = { {
        { level0, 32, 0 },
        { level1, 16, 0 },
        { level0, 32, 0 },
        { level1, 16, 0 },
    } };
    CHECK(device
              .createTexture(
                  { .type = TextureType::Tex2DArray,
                    .format = Format::RGBA8Unorm,
                    .extent = { 8, 8, 1 },
                    .mips = 2,
                    .layers = 2,
                    .usage = TextureUsage::Sampled },
                  data
              )
              .has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("initial data may pad its rows, leave a subresource empty and describe a volume or compressed blocks", "[contract]")
{
    Device device = makeDevice();
    const std::vector<std::byte> padded(64 * 3 + 16);
    const TextureData rows{ padded, 64, 0 };
    CHECK(device.createTexture({ .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled }, std::span(&rows, 1)).has_value());
    const TextureData undefined{};
    CHECK(device.createTexture({ .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled }, std::span(&undefined, 1)).has_value());
    const std::vector<std::byte> volume(4 * 2 * 4 * 2);
    const TextureData slices{ volume, 8, 16 };
    CHECK(device
              .createTexture(
                  { .type = TextureType::Tex3D, .format = Format::R16Float, .extent = { 4, 2, 4 }, .usage = TextureUsage::Sampled }, std::span(&slices, 1)
              )
              .has_value());
    const std::vector<std::byte> blocks(8);
    const TextureData compressed{ blocks, 8, 0 };
    CHECK(device.createTexture({ .format = Format::BC1Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled }, std::span(&compressed, 1)).has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("placement requirements of a texture have a power-of-two alignment and are repeatable", "[contract]")
{
    Device device = makeDevice();
    const TextureDesc desc{ .format = Format::RGBA8Unorm, .extent = { 33, 17, 1 }, .mips = 3, .usage = TextureUsage::Sampled };
    const PlacementRequirements first = device.placementRequirements(desc);
    const PlacementRequirements second = device.placementRequirements(desc);
    CHECK(isPowerOfTwo(first.alignment));
    CHECK(first.size >= 33 * 17 * 4);
    CHECK(first.size == second.size);
    CHECK(first.alignment == second.alignment);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a texture is placed in a Gpu heap at its required alignment and the heap is destroyed after it", "[contract]")
{
    Device device = makeDevice();
    const TextureDesc desc{ .format = Format::RGBA8Unorm, .extent = { 32, 32, 1 }, .usage = TextureUsage::Sampled };
    const PlacementRequirements requirements = device.placementRequirements(desc);
    const Heap heap = *device.createHeap({ .size = requirements.size * 2 + requirements.alignment });
    const Texture first = makeTexture(device, desc, {}, { heap, 0 });
    const Texture second = makeTexture(
        device, desc, {}, { heap, requirements.size + (requirements.alignment - requirements.size % requirements.alignment) % requirements.alignment }
    );
    device.destroy(first);
    device.destroy(second);
    device.destroy(heap);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("views of every type are created", "[contract]")
{
    Device device = makeDevice();
    const Texture cube = makeTexture(
        device, { .type = TextureType::CubeArray, .format = Format::RGBA8Unorm, .extent = { 8, 8, 1 }, .mips = 2, .layers = 12, .usage = TextureUsage::Sampled }
    );
    CHECK(device.createView(cube, { .type = TextureType::CubeArray }).has_value());
    CHECK(device.createView(cube, { .type = TextureType::Cube, .range = { .baseLayer = 6, .layerCount = 6 } }).has_value());
    CHECK(device.createView(cube, { .type = TextureType::Tex2DArray }).has_value());
    CHECK(device.createView(cube, { .type = TextureType::Tex2D, .range = { .baseMip = 1, .mipCount = 1, .baseLayer = 3, .layerCount = 1 } }).has_value());
    const Texture volume = makeTexture(
        device, { .type = TextureType::Tex3D, .format = Format::RGBA8Unorm, .extent = { 4, 4, 4 }, .usage = TextureUsage::Sampled | TextureUsage::Storage }
    );
    CHECK(device.createView(volume, { .type = TextureType::Tex3D }).has_value());
    CHECK(device.createView(volume, { .type = TextureType::Tex3D, .usage = TextureUsage::Storage }).has_value());
    const Texture line =
        makeTexture(device, { .type = TextureType::Tex1DArray, .format = Format::R8Unorm, .extent = { 8, 1, 1 }, .layers = 4, .usage = TextureUsage::Sampled });
    CHECK(device.createView(line, { .type = TextureType::Tex1D, .range = { .baseLayer = 2, .layerCount = 1 } }).has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a view reinterprets an sRGB format within its family and selects the planes of a depth-stencil format", "[contract]")
{
    Device device = makeDevice();
    const Texture color = makeTexture(device, { .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled });
    CHECK(device.createView(color, { .format = Format::RGBA8Unorm }).has_value());
    CHECK(device.createView(color, { .format = Format::RGBA8Unorm_sRGB }).has_value());
    const Texture depth =
        makeTexture(device, { .format = Format::D32FloatS8, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled | TextureUsage::DepthStencil });
    CHECK(device.createView(depth, { .aspect = TextureAspect::Depth }).has_value());
    CHECK(device.createView(depth, { .aspect = TextureAspect::Stencil }).has_value());
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("two live views have different indices", "[contract]")
{
    Device device = makeDevice();
    const Texture texture = makeTexture(device, { .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled });
    std::set<ViewIndex> seen;
    for (int i = 0; i < 16; ++i)
    {
        CHECK(seen.insert(makeView(device, texture)).second);
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a destroyed view's index is not handed out before the frame that destroyed it retires", "[contract]")
{
    Device device = makeDevice();
    const Texture texture = makeTexture(device, { .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled });
    device.beginFrame();
    const ViewIndex released = makeView(device, texture);
    device.destroy(released);
    for (int i = 0; i < 8; ++i)
    {
        CHECK(makeView(device, texture) != released);
    }
    device.endFrame();
    device.beginFrame();
    for (int i = 0; i < 8; ++i)
    {
        CHECK(makeView(device, texture) != released);
    }
    device.endFrame();
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a texture is destroyed once its views are destroyed", "[contract]")
{
    Device device = makeDevice();
    const Texture texture = makeTexture(device, { .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled });
    const ViewIndex first = makeView(device, texture);
    const ViewIndex second = makeView(device, texture);
    device.destroy(first);
    device.destroy(second);
    device.destroy(texture);
    destroyDevice(device);
}
