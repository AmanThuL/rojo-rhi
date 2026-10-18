//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4TextureTests.cpp
/// @brief Tests Metal 4 textures, views and samplers against the native objects and the view and sampler tables.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Formats.h"
#include "Core/Records.h"
#include "Core/Textures.h"
#include "Metal4Common.h"
#include "Metal4Device.h"
#include "Metal4TextureDescriptor.h"
#include "Metal4Views.h"
#include "Support/Metal4/Metal4Readback.h"

#include <catch2/catch_test_macros.hpp>

#include <Metal/Metal.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    /// The native texture `texture` wraps.
    MTL::Texture* nativeOf(Device device, Texture texture)
    {
        return static_cast<MTL::Texture*>(DeviceImpl::of(device).core.pool<Texture>().native(texture));
    }

    //==================================================================================================================
    /// The native heap `heap` wraps.
    MTL::Heap* nativeOf(Device device, Heap heap)
    {
        return static_cast<MTL::Heap*>(DeviceImpl::of(device).core.pool<Heap>().native(heap));
    }

    //==================================================================================================================
    /// The resource ID the view table holds at `slot`.
    uint64_t viewEntry(Device device, uint32_t slot)
    {
        return static_cast<const MTL::ResourceID*>(DeviceImpl::of(device).viewTable->contents())[slot]._impl;
    }

    //==================================================================================================================
    /// The resource ID the sampler table holds at `slot`.
    uint64_t samplerEntry(Device device, uint32_t slot)
    {
        return static_cast<const MTL::ResourceID*>(DeviceImpl::of(device).samplerTable->contents())[slot]._impl;
    }

    //==================================================================================================================
    /// Runs `count` empty frames on `device`.
    void runFrames(Device device, uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            device.beginFrame();
            device.endFrame();
        }
    }

    /// One subresource of initial data: the bytes handed to `createTexture`, with the strides the case chose, and the tight
    /// rows a readback of it returns.
    struct Subresource
    {
        std::vector<std::byte> bytes;      ///< What `TextureData::bytes` views, padding included.
        uint64_t bytesPerRow = 0;          ///< The stride between rows of texels or blocks.
        uint64_t bytesPerImage = 0;        ///< The stride between slices; nonzero for Tex3D only.
        std::vector<std::byte> tight = {}; ///< The same texels with no padding, as `test::readTexture` returns them.
        uint64_t minimumSize = 0;          ///< The bytes chapter 04 requires: the last row and slice unpadded.
    };

    //==================================================================================================================
    /// Builds subresource `mip` of a texture of `format` and mip-0 `extent`, every texel byte distinct per `seed`, with
    /// `rowPadding` bytes after each row and `slicePadding` bytes after each slice of a volume.
    Subresource subresource(Format format, Extent3D extent, uint32_t mip, uint32_t seed, uint64_t rowPadding, uint64_t slicePadding, bool volume)
    {
        const core::FormatLayout layout = core::formatLayout(format);
        const Extent3D size = core::mipExtent(extent, mip);
        const uint64_t packedRow = (uint64_t{ size.width } + layout.blockWidth - 1) / layout.blockWidth * layout.blockBytes;
        const uint64_t rows = (uint64_t{ size.height } + layout.blockHeight - 1) / layout.blockHeight;
        const uint64_t slices = volume ? size.depth : 1;
        Subresource result;
        result.bytesPerRow = packedRow + rowPadding;
        result.bytesPerImage = volume ? result.bytesPerRow * rows + slicePadding : 0;
        const uint64_t imageStride = volume ? result.bytesPerImage : result.bytesPerRow * rows;
        result.bytes.assign(imageStride * slices, std::byte{ 0xEE });
        result.minimumSize = (volume ? result.bytesPerImage * (slices - 1) : 0) + result.bytesPerRow * (rows - 1) + packedRow;
        uint32_t counter = seed * 7919u;
        for (uint64_t slice = 0; slice < slices; ++slice)
        {
            for (uint64_t row = 0; row < rows; ++row)
            {
                for (uint64_t column = 0; column < packedRow; ++column)
                {
                    const auto value = static_cast<std::byte>(static_cast<uint8_t>(++counter * 31u + seed));
                    result.bytes[slice * imageStride + row * result.bytesPerRow + column] = value;
                    result.tight.push_back(value);
                }
            }
        }
        return result;
    }

    //==================================================================================================================
    /// The `TextureData` entries that view `subresources`, in their order.
    std::vector<TextureData> dataOf(const std::vector<Subresource>& subresources)
    {
        std::vector<TextureData> data;
        for (const Subresource& item : subresources)
        {
            data.push_back(TextureData{ item.bytes, item.bytesPerRow, item.bytesPerImage });
        }
        return data;
    }

    /// The view and sampler payloads that retired while a spy was installed, each retained so a case can inspect it.
    struct Retired
    {
        std::vector<MTL::Texture*> views;         ///< Each view payload handed to the `view` hook, in order.
        std::vector<MTL::SamplerState*> samplers; ///< Each sampler payload handed to the `sampler` hook, in order.
    };

    /// The hooks the spy wraps and what it saw; one device is spied on at a time.
    core::RetireHooks g_original;
    Retired g_retired;

    //==================================================================================================================
    /// Keeps a retained reference to a retiring view payload, then runs the wrapped hook.
    void spyView(void* context, void* native)
    {
        static_cast<MTL::Texture*>(native)->retain();
        g_retired.views.push_back(static_cast<MTL::Texture*>(native));
        g_original.view(context, native);
    }

    //==================================================================================================================
    /// Keeps a retained reference to a retiring sampler payload, then runs the wrapped hook.
    void spySampler(void* context, void* native)
    {
        static_cast<MTL::SamplerState*>(native)->retain();
        g_retired.samplers.push_back(static_cast<MTL::SamplerState*>(native));
        g_original.sampler(context, native);
    }

    //==================================================================================================================
    /// Wraps the `view` and `sampler` retire hooks of `device`, keeping each payload they release in `g_retired`.
    void spyOnRetirement(Device device)
    {
        core::DeviceCore& core = core::coreOf(device);
        g_original = core.retireHooks();
        g_retired = {};
        core::RetireHooks hooks = g_original;
        hooks.view = spyView;
        hooks.sampler = spySampler;
        core.setRetireHooks(hooks);
    }

    //==================================================================================================================
    /// Releases every payload the spy kept and forgets them.
    void releaseRetired()
    {
        const metal4::AutoreleasePool pool;
        for (MTL::Texture* view : g_retired.views)
        {
            view->release();
        }
        for (MTL::SamplerState* sampler : g_retired.samplers)
        {
            sampler->release();
        }
        g_retired = {};
    }

    //==================================================================================================================
    /// Destroys `view` and runs frames until its slot retires; returns the view object it held, which the spy keeps.
    MTL::Texture* retireView(Device device, ViewIndex view)
    {
        const size_t before = g_retired.views.size();
        device.destroy(view);
        runFrames(device, core::coreOf(device).framesInFlight() + 1);
        REQUIRE(g_retired.views.size() == before + 1);
        return g_retired.views.back();
    }
} // namespace

//======================================================================================================================
TEST_CASE("a texture's initial data is in every mip and layer when createTexture returns", "[metal4]")
{
    Device device = *createDevice();
    const TextureDesc desc{
        .type = TextureType::Tex2DArray, .format = Format::RGBA8Unorm, .extent = { 16, 8, 1 }, .mips = 3, .layers = 2, .usage = TextureUsage::Sampled
    };
    std::vector<Subresource> subresources;
    for (uint32_t layer = 0; layer < desc.layers; ++layer)
    {
        for (uint32_t mip = 0; mip < desc.mips; ++mip)
        {
            // Mip 0 pads its rows; the others are tight.
            subresources.push_back(subresource(desc.format, desc.extent, mip, layer * 10 + mip + 1, mip == 0 ? 24 : 0, 0, false));
        }
    }
    const std::vector<TextureData> data = dataOf(subresources);
    const Texture texture = *device.createTexture(desc, data);
    for (uint32_t layer = 0; layer < desc.layers; ++layer)
    {
        for (uint32_t mip = 0; mip < desc.mips; ++mip)
        {
            CAPTURE(layer, mip);
            const std::vector<std::byte> read = test::readTexture(device, texture, mip, layer);
            const Subresource& expected = subresources[mip + layer * desc.mips];
            REQUIRE(read.size() == expected.tight.size());
            CHECK(std::memcmp(read.data(), expected.tight.data(), read.size()) == 0);
        }
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("initial data of the minimum size, its last row and slice unpadded, arrives in full", "[metal4]")
{
    Device device = *createDevice();
    const TextureDesc arrayDesc{
        .type = TextureType::Tex2DArray, .format = Format::RGBA8Unorm, .extent = { 8, 8, 1 }, .mips = 2, .layers = 2, .usage = TextureUsage::Sampled
    };
    std::vector<Subresource> subresources;
    for (uint32_t layer = 0; layer < arrayDesc.layers; ++layer)
    {
        for (uint32_t mip = 0; mip < arrayDesc.mips; ++mip)
        {
            subresources.push_back(subresource(arrayDesc.format, arrayDesc.extent, mip, 150 + layer * 10 + mip, 24, 0, false));
        }
    }
    // The last entry is staged last, so nothing follows it in the staging buffer.
    subresources.back().bytes.resize(subresources.back().minimumSize);
    const std::vector<TextureData> arrayData = dataOf(subresources);
    const Texture array = *device.createTexture(arrayDesc, arrayData);
    for (uint32_t layer = 0; layer < arrayDesc.layers; ++layer)
    {
        for (uint32_t mip = 0; mip < arrayDesc.mips; ++mip)
        {
            CAPTURE(layer, mip);
            CHECK(test::readTexture(device, array, mip, layer) == subresources[mip + layer * arrayDesc.mips].tight);
        }
    }
    const TextureDesc volumeDesc{ .type = TextureType::Tex3D, .format = Format::RGBA8Unorm, .extent = { 4, 4, 4 }, .usage = TextureUsage::Sampled };
    Subresource volume = subresource(volumeDesc.format, volumeDesc.extent, 0, 170, 8, 32, true);
    volume.bytes.resize(volume.minimumSize);
    const TextureData volumeData{ volume.bytes, volume.bytesPerRow, volume.bytesPerImage };
    const Texture texture = *device.createTexture(volumeDesc, std::span(&volumeData, 1));
    CHECK(test::readTexture(device, texture, 0, 0) == volume.tight);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a cube texture's faces and a volume's slices receive their initial data", "[metal4]")
{
    Device device = *createDevice();
    const TextureDesc cubeDesc{
        .type = TextureType::CubeArray, .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .layers = 12, .usage = TextureUsage::Sampled
    };
    std::vector<Subresource> faces;
    for (uint32_t face = 0; face < cubeDesc.layers; ++face)
    {
        faces.push_back(subresource(cubeDesc.format, cubeDesc.extent, 0, face + 1, 0, 0, false));
    }
    const std::vector<TextureData> faceData = dataOf(faces);
    const Texture cubes = *device.createTexture(cubeDesc, faceData);
    CHECK(nativeOf(device, cubes)->textureType() == MTL::TextureTypeCubeArray);
    CHECK(nativeOf(device, cubes)->arrayLength() == 2);
    for (uint32_t face = 0; face < cubeDesc.layers; ++face)
    {
        CAPTURE(face);
        CHECK(test::readTexture(device, cubes, 0, face) == faces[face].tight);
    }
    const TextureDesc volumeDesc{ .type = TextureType::Tex3D, .format = Format::RGBA8Unorm, .extent = { 4, 4, 4 }, .mips = 2, .usage = TextureUsage::Sampled };
    std::vector<Subresource> mips;
    for (uint32_t mip = 0; mip < volumeDesc.mips; ++mip)
    {
        // Rows and slices are both padded.
        mips.push_back(subresource(volumeDesc.format, volumeDesc.extent, mip, 40 + mip, 8, 32, true));
    }
    const std::vector<TextureData> volumeData = dataOf(mips);
    const Texture volume = *device.createTexture(volumeDesc, volumeData);
    for (uint32_t mip = 0; mip < volumeDesc.mips; ++mip)
    {
        CAPTURE(mip);
        CHECK(test::readTexture(device, volume, mip, 0) == mips[mip].tight);
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a BC1 texture's blocks arrive when the device supports BC", "[metal4]")
{
    Device device = *createDevice();
    if (!device.formatCapabilities(Format::BC1Unorm).sampled)
    {
        destroyDevice(device);
        SKIP("the device does not support BC texture compression");
    }
    // 8 by 8 texels are 2 by 2 blocks at mip 0; mip 1 is one block and mip 2, 2 by 2 texels, a partial block.
    const TextureDesc desc{ .format = Format::BC1Unorm, .extent = { 8, 8, 1 }, .mips = 3, .usage = TextureUsage::Sampled };
    std::vector<Subresource> subresources;
    for (uint32_t mip = 0; mip < desc.mips; ++mip)
    {
        subresources.push_back(subresource(desc.format, desc.extent, mip, 70 + mip, mip == 0 ? 16 : 0, 0, false));
    }
    const std::vector<TextureData> data = dataOf(subresources);
    const Texture texture = *device.createTexture(desc, data);
    for (uint32_t mip = 0; mip < desc.mips; ++mip)
    {
        CAPTURE(mip);
        CHECK(test::readTexture(device, texture, mip, 0) == subresources[mip].tight);
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a placed texture's initial data arrives and the texture stays out of the residency set", "[metal4]")
{
    Device device = *createDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    const TextureDesc desc{ .format = Format::RGBA8Unorm, .extent = { 16, 16, 1 }, .mips = 2, .usage = TextureUsage::Sampled };
    const PlacementRequirements requirements = device.placementRequirements(desc);
    const Heap heap = *device.createHeap({ .size = requirements.size * 2 + requirements.alignment });
    const uint64_t offset = (requirements.size + requirements.alignment - 1) / requirements.alignment * requirements.alignment;
    std::vector<Subresource> subresources;
    for (uint32_t mip = 0; mip < desc.mips; ++mip)
    {
        subresources.push_back(subresource(desc.format, desc.extent, mip, 90 + mip, 0, 0, false));
    }
    const std::vector<TextureData> data = dataOf(subresources);
    const Texture placed = *device.createTexture(desc, data, { heap, offset });
    MTL::Texture* native = nativeOf(device, placed);
    CHECK(native->heap() == nativeOf(device, heap));
    CHECK(native->heapOffset() == offset);
    CHECK(native->storageMode() == MTL::StorageModePrivate);
    CHECK(native->hazardTrackingMode() == MTL::HazardTrackingModeUntracked);
    CHECK_FALSE(impl.residency.contains(native));
    CHECK(impl.residency.contains(nativeOf(device, heap)));
    for (uint32_t mip = 0; mip < desc.mips; ++mip)
    {
        CAPTURE(mip);
        CHECK(test::readTexture(device, placed, mip, 0) == subresources[mip].tight);
    }
    const Texture dedicated = *device.createTexture(desc);
    CHECK(nativeOf(device, dedicated)->storageMode() == MTL::StorageModePrivate);
    CHECK(impl.residency.contains(nativeOf(device, dedicated)));
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a dedicated texture leaves the residency set when its frame retires", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    DeviceImpl& impl = DeviceImpl::of(device);
    const Texture texture = *device.createTexture({ .format = Format::R8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled });
    MTL::Texture* native = nativeOf(device, texture);
    // Kept alive past retirement, so the set can still be asked about it.
    native->retain();
    device.destroy(texture);
    runFrames(device, 2);
    CHECK(impl.residency.contains(native));
    runFrames(device, 1);
    CHECK_FALSE(impl.residency.contains(native));
    {
        const metal4::AutoreleasePool pool;
        native->release();
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a texture's descriptor maps its type, extent, layers, samples and usage", "[metal4]")
{
    const metal4::AutoreleasePool pool;
    CHECK(metal4::textureType(TextureType::Tex2D, 1) == MTL::TextureType2D);
    CHECK(metal4::textureType(TextureType::Tex2D, 4) == MTL::TextureType2DMultisample);
    CHECK(metal4::textureType(TextureType::Tex2DArray, 4) == MTL::TextureType2DMultisampleArray);
    CHECK(metal4::textureType(TextureType::Cube, 1) == MTL::TextureTypeCube);
    CHECK(metal4::textureType(TextureType::Tex1DArray, 1) == MTL::TextureType1DArray);
    MTL::TextureDescriptor* cube = metal4::textureDescriptor(
        { .type = TextureType::CubeArray, .format = Format::RGBA8Unorm_sRGB, .extent = { 8, 8, 1 }, .mips = 4, .layers = 18, .usage = TextureUsage::Sampled },
        false
    );
    CHECK(cube->textureType() == MTL::TextureTypeCubeArray);
    CHECK(cube->pixelFormat() == MTL::PixelFormatRGBA8Unorm_sRGB);
    CHECK(cube->arrayLength() == 3);
    CHECK(cube->mipmapLevelCount() == 4);
    CHECK(cube->storageMode() == MTL::StorageModePrivate);
    CHECK((cube->usage() & MTL::TextureUsageShaderRead) != 0);
    CHECK((cube->usage() & MTL::TextureUsagePixelFormatView) != 0);
    CHECK((cube->usage() & (MTL::TextureUsageShaderWrite | MTL::TextureUsageRenderTarget)) == 0);
    cube->release();
    MTL::TextureDescriptor* target = metal4::textureDescriptor(
        { .type = TextureType::Tex2DArray,
          .format = Format::RGBA16Float,
          .extent = { 32, 16, 1 },
          .layers = 3,
          .samples = 4,
          .usage = TextureUsage::RenderTarget },
        true
    );
    CHECK(target->textureType() == MTL::TextureType2DMultisampleArray);
    CHECK(target->width() == 32);
    CHECK(target->height() == 16);
    CHECK(target->depth() == 1);
    CHECK(target->arrayLength() == 3);
    CHECK(target->sampleCount() == 4);
    CHECK(target->usage() == MTL::TextureUsageRenderTarget);
    CHECK(target->hazardTrackingMode() == MTL::HazardTrackingModeUntracked);
    target->release();
    MTL::TextureDescriptor* storage =
        metal4::textureDescriptor({ .type = TextureType::Tex3D, .format = Format::R32Float, .extent = { 8, 4, 2 }, .usage = TextureUsage::Storage }, false);
    CHECK(storage->depth() == 2);
    CHECK(storage->arrayLength() == 1);
    CHECK((storage->usage() & (MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite)) == (MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite));
    storage->release();
}

//======================================================================================================================
TEST_CASE("a view of the whole texture stores the texture's own resource ID", "[metal4]")
{
    Device device = *createDevice();
    const Texture texture = *device.createTexture({ .format = Format::RGBA8Unorm, .extent = { 8, 8, 1 }, .mips = 4, .usage = TextureUsage::Sampled });
    const ViewIndex view = *device.createView(texture, {});
    const uint64_t id = nativeOf(device, texture)->gpuResourceID()._impl;
    CHECK(id != 0);
    CHECK(viewEntry(device, std::to_underlying(view)) == id);
    // A Storage view of one mip of a one-mip texture is the texture too.
    const Texture storage = *device.createTexture({ .format = Format::RGBA8Unorm, .extent = { 8, 8, 1 }, .usage = TextureUsage::Storage });
    const ViewIndex storageView = *device.createView(storage, { .usage = TextureUsage::Storage });
    CHECK(viewEntry(device, std::to_underlying(storageView)) == nativeOf(device, storage)->gpuResourceID()._impl);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a view of one mip stores a new view's resource ID", "[metal4]")
{
    Device device = *createDevice();
    spyOnRetirement(device);
    const Texture texture = *device.createTexture({ .format = Format::RGBA16Float, .extent = { 8, 8, 1 }, .mips = 4, .usage = TextureUsage::Sampled });
    MTL::Texture* parent = nativeOf(device, texture);
    // Neither format has an sRGB partner or a stencil plane, so views of a mip or a face need no pixel-format views.
    CHECK((parent->usage() & MTL::TextureUsagePixelFormatView) == 0);
    const ViewIndex view = *device.createView(texture, { .range = { .baseMip = 1, .mipCount = 1 } });
    const uint64_t entry = viewEntry(device, std::to_underlying(view));
    CHECK(entry != 0);
    CHECK(entry != parent->gpuResourceID()._impl);
    const Texture cube = *device.createTexture(
        { .type = TextureType::CubeArray, .format = Format::RGBA16Float, .extent = { 4, 4, 1 }, .layers = 12, .usage = TextureUsage::Sampled }
    );
    const ViewIndex face = *device.createView(cube, { .type = TextureType::Tex2D, .range = { .baseLayer = 9, .layerCount = 1 } });
    const uint64_t faceEntry = viewEntry(device, std::to_underlying(face));
    MTL::Texture* native = retireView(device, view);
    CHECK(native != parent);
    CHECK(native->parentTexture() == parent);
    CHECK(native->gpuResourceID()._impl == entry);
    CHECK(native->textureType() == MTL::TextureType2D);
    CHECK(native->mipmapLevelCount() == 1);
    CHECK(native->parentRelativeLevel() == 1);
    CHECK(native->width() == 4);
    MTL::Texture* faceView = retireView(device, face);
    CHECK(faceView->gpuResourceID()._impl == faceEntry);
    CHECK(faceView->textureType() == MTL::TextureType2D);
    CHECK(faceView->parentRelativeSlice() == 9);
    CHECK(faceView->mipmapLevelCount() == 1);
    destroyDevice(device);
    releaseRetired();
}

//======================================================================================================================
TEST_CASE("a stencil view of D32FloatS8 is an X32_Stencil8 view", "[metal4]")
{
    Device device = *createDevice();
    spyOnRetirement(device);
    const Texture texture =
        *device.createTexture({ .format = Format::D32FloatS8, .extent = { 8, 8, 1 }, .usage = TextureUsage::Sampled | TextureUsage::DepthStencil });
    MTL::Texture* parent = nativeOf(device, texture);
    CHECK((parent->usage() & MTL::TextureUsagePixelFormatView) != 0);
    const ViewIndex depth = *device.createView(texture, { .aspect = TextureAspect::Depth });
    CHECK(viewEntry(device, std::to_underlying(depth)) == parent->gpuResourceID()._impl);
    const ViewIndex stencil = *device.createView(texture, { .aspect = TextureAspect::Stencil });
    const uint64_t entry = viewEntry(device, std::to_underlying(stencil));
    MTL::Texture* native = retireView(device, stencil);
    CHECK(native->pixelFormat() == MTL::PixelFormatX32_Stencil8);
    CHECK(native->parentTexture() == parent);
    CHECK(native->gpuResourceID()._impl == entry);
    destroyDevice(device);
    releaseRetired();
}

//======================================================================================================================
TEST_CASE("an sRGB texture has a linear view in its family", "[metal4]")
{
    Device device = *createDevice();
    spyOnRetirement(device);
    const Texture texture = *device.createTexture({ .format = Format::RGBA8Unorm_sRGB, .extent = { 8, 8, 1 }, .usage = TextureUsage::Sampled });
    MTL::Texture* parent = nativeOf(device, texture);
    CHECK((parent->usage() & MTL::TextureUsagePixelFormatView) != 0);
    const ViewIndex linear = *device.createView(texture, { .format = Format::RGBA8Unorm });
    const uint64_t entry = viewEntry(device, std::to_underlying(linear));
    CHECK(entry != parent->gpuResourceID()._impl);
    MTL::Texture* native = retireView(device, linear);
    CHECK(native->pixelFormat() == MTL::PixelFormatRGBA8Unorm);
    CHECK(native->parentTexture() == parent);
    CHECK(native->gpuResourceID()._impl == entry);
    destroyDevice(device);
    releaseRetired();
}

//======================================================================================================================
TEST_CASE("writeView replaces a slot's view and its old view is released when the frame retires", "[metal4]")
{
    Device device = *createDevice({ .framesInFlight = 2 });
    spyOnRetirement(device);
    const TextureDesc desc{ .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled };
    const Texture first = *device.createTexture(desc);
    const Texture second = *device.createTexture(desc);
    const IndexRange range = *device.reserveViews(3);
    device.writeView(range, 1, first, {});
    CHECK(viewEntry(device, range.base + 1) == nativeOf(device, first)->gpuResourceID()._impl);
    CHECK(viewEntry(device, range.base) == 0);
    device.writeView(range, 1, second, {});
    CHECK(viewEntry(device, range.base + 1) == nativeOf(device, second)->gpuResourceID()._impl);
    // The replaced view waits for frame 1, which retires when frame 3 begins.
    CHECK(g_retired.views.empty());
    runFrames(device, 2);
    CHECK(g_retired.views.empty());
    runFrames(device, 1);
    REQUIRE(g_retired.views.size() == 1);
    // A whole-texture view is the texture itself, retained.
    CHECK(g_retired.views[0] == nativeOf(device, first));
    device.destroy(first);
    device.release(range);
    runFrames(device, 3);
    REQUIRE(g_retired.views.size() == 2);
    CHECK(g_retired.views[1] == nativeOf(device, second));
    destroyDevice(device);
    releaseRetired();
}

//======================================================================================================================
TEST_CASE("a sampler stores its resource ID in the sampler table and honours compare and anisotropy", "[metal4]")
{
    Device device = *createDevice();
    spyOnRetirement(device);
    const SamplerDesc desc{
        .filter = SamplerFilter::Linear,
        .mipFilter = SamplerFilter::Linear,
        .address = SamplerAddress::MirrorRepeat,
        .maxAnisotropy = 8,
        .compareEnable = true,
        .compare = CompareOp::GreaterOrEqual,
        .label = "test.shadow",
    };
    {
        const metal4::AutoreleasePool pool;
        MTL::SamplerDescriptor* descriptor = metal4::samplerDescriptor(desc, "test.shadow");
        CHECK(descriptor->minFilter() == MTL::SamplerMinMagFilterLinear);
        CHECK(descriptor->magFilter() == MTL::SamplerMinMagFilterLinear);
        CHECK(descriptor->mipFilter() == MTL::SamplerMipFilterLinear);
        CHECK(descriptor->sAddressMode() == MTL::SamplerAddressModeMirrorRepeat);
        CHECK(descriptor->tAddressMode() == MTL::SamplerAddressModeMirrorRepeat);
        CHECK(descriptor->rAddressMode() == MTL::SamplerAddressModeMirrorRepeat);
        CHECK(descriptor->maxAnisotropy() == 8);
        CHECK(descriptor->compareFunction() == MTL::CompareFunctionGreaterEqual);
        CHECK(descriptor->supportArgumentBuffers());
        CHECK(metal4::toStdString(descriptor->label()) == "test.shadow");
        descriptor->release();
        MTL::SamplerDescriptor* plain =
            metal4::samplerDescriptor({ .filter = SamplerFilter::Nearest, .mipFilter = SamplerFilter::Nearest, .address = SamplerAddress::ClampToEdge }, "x");
        CHECK(plain->minFilter() == MTL::SamplerMinMagFilterNearest);
        CHECK(plain->mipFilter() == MTL::SamplerMipFilterNearest);
        CHECK(plain->sAddressMode() == MTL::SamplerAddressModeClampToEdge);
        CHECK(plain->maxAnisotropy() == 1);
        CHECK(plain->compareFunction() == MTL::CompareFunctionNever);
        plain->release();
    }
    const SamplerIndex sampler = *device.createSampler(desc);
    const uint64_t entry = samplerEntry(device, std::to_underlying(sampler));
    CHECK(entry != 0);
    const IndexRange range = *device.reserveSamplers(2);
    device.writeSampler(range, 1, {});
    const uint64_t written = samplerEntry(device, range.base + 1);
    CHECK(written != 0);
    CHECK(written != entry);
    device.destroy(sampler);
    device.release(range);
    runFrames(device, core::coreOf(device).framesInFlight() + 1);
    REQUIRE(g_retired.samplers.size() == 2);
    CHECK(g_retired.samplers[0]->gpuResourceID()._impl == entry);
    CHECK(g_retired.samplers[1]->gpuResourceID()._impl == written);
    destroyDevice(device);
    releaseRetired();
}

//======================================================================================================================
TEST_CASE("placement requirements of a texture are the device's size and alignment for the placed descriptor", "[metal4]")
{
    Device device = *createDevice();
    MTL::Device* native = DeviceImpl::of(device).device;
    const metal4::AutoreleasePool pool;
    for (const TextureDesc& desc : {
             TextureDesc{ .format = Format::RGBA8Unorm, .extent = { 33, 17, 1 }, .mips = 3, .usage = TextureUsage::Sampled },
             TextureDesc{ .type = TextureType::Cube, .format = Format::RGBA16Float, .extent = { 64, 64, 1 }, .layers = 6, .usage = TextureUsage::Sampled },
             TextureDesc{ .format = Format::D32Float, .extent = { 128, 64, 1 }, .samples = 4, .usage = TextureUsage::DepthStencil },
         })
    {
        CAPTURE(static_cast<int>(desc.format));
        const PlacementRequirements requirements = device.placementRequirements(desc);
        MTL::TextureDescriptor* descriptor = metal4::textureDescriptor(desc, true);
        const MTL::SizeAndAlign expected = native->heapTextureSizeAndAlign(descriptor);
        descriptor->release();
        CHECK(requirements.size == expected.size);
        CHECK(requirements.alignment == expected.align);
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a staged texture counts one staging buffer and one submission, and empty initial data stages nothing", "[metal4]")
{
    Device device = *createDevice();
    const TextureDesc desc{ .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .mips = 2, .usage = TextureUsage::Sampled };
    std::vector<Subresource> subresources;
    for (uint32_t mip = 0; mip < desc.mips; ++mip)
    {
        subresources.push_back(subresource(desc.format, desc.extent, mip, 120 + mip, 0, 0, false));
    }
    const std::vector<TextureData> data = dataOf(subresources);
    const std::vector<TextureData> empty(desc.mips);
    const Counters start = device.counters();
    (void)*device.createTexture(desc);
    const Counters plain = device.counters();
    (void)*device.createTexture(desc, empty);
    const Counters undefined = device.counters();
    (void)*device.createTexture(desc, data);
    const Counters staged = device.counters();
    CHECK(plain.nativeSubmitCalls == start.nativeSubmitCalls);
    CHECK(undefined.nativeSubmitCalls == plain.nativeSubmitCalls);
    CHECK(undefined.nativeCreateCalls - plain.nativeCreateCalls == plain.nativeCreateCalls - start.nativeCreateCalls);
    CHECK(staged.nativeSubmitCalls == undefined.nativeSubmitCalls + 1);
    CHECK(staged.nativeCreateCalls - undefined.nativeCreateCalls == plain.nativeCreateCalls - start.nativeCreateCalls + 1);
    // The new texture joined the device set, which the staging commit makes resident first; the staging buffer joins and
    // leaves the stager's own set, one commit each.
    CHECK(staged.nativeHostCalls == undefined.nativeHostCalls + 3);
    destroyDevice(device);
}
