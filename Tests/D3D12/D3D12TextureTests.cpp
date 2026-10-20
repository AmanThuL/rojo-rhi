//----------------------------------------------------------------------------------------------------------------------
/// @file D3D12TextureTests.cpp
/// @brief Tests D3D12 textures, their render-target and depth-stencil caches, views and samplers on the native objects.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/DeviceCore.h"
#include "Core/Records.h"
#include "D3D12Common.h"
#include "D3D12Descriptors.h"
#include "D3D12Device.h"
#include "D3D12Textures.h"
#include "D3D12Views.h"
#include "Support/D3D12/D3D12Readback.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
    /// Returns a new device with the default description.
    Device makeDevice()
    {
        Result<Device> created = createDevice();
        REQUIRE(created.has_value());
        return *created;
    }

    //==================================================================================================================
    /// Returns a new texture of `desc` on `device`.
    Texture makeTexture(Device device, const TextureDesc& desc, std::span<const TextureData> initial = {})
    {
        Result<Texture> created = device.createTexture(desc, initial);
        REQUIRE(created.has_value());
        return *created;
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
    /// Returns the description `resource` was created with.
    D3D12_RESOURCE_DESC1 descOf(ID3D12Resource* resource)
    {
        d3d12::Com<ID3D12Resource2> described;
        REQUIRE(SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(described.put()))));
        return described->GetDesc1();
    }

    //==================================================================================================================
    /// Returns the references `resource` holds, read by taking one and giving it back.
    ULONG referencesOf(ID3D12Resource* resource)
    {
        resource->AddRef();
        return resource->Release();
    }

    //==================================================================================================================
    /// Runs `count` frames on `device`, with no work in them.
    void emptyFrames(Device device, uint32_t count)
    {
        for (uint32_t frame = 0; frame < count; ++frame)
        {
            device.beginFrame();
            device.endFrame();
        }
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

    /// The shape of one texture type and the resource D3D12 makes of it.
    struct TypeShape
    {
        TextureType type = TextureType::Tex2D;                                   ///< The texture's type.
        Extent3D extent = {};                                                    ///< Its mip-0 extent.
        uint32_t layers = 1;                                                     ///< Its layers, faces for cube types.
        D3D12_RESOURCE_DIMENSION dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; ///< The resource's dimension.
        UINT16 depthOrArraySize = 1;                                             ///< The resource's depth or array size.
    };
} // namespace

//======================================================================================================================
TEST_CASE("every texture type creates its D3D12 resource", "[d3d12]")
{
    const TypeShape shapes[] = {
        { TextureType::Tex1D, { 16, 1, 1 }, 1, D3D12_RESOURCE_DIMENSION_TEXTURE1D, 1 },
        { TextureType::Tex1DArray, { 16, 1, 1 }, 3, D3D12_RESOURCE_DIMENSION_TEXTURE1D, 3 },
        { TextureType::Tex2D, { 16, 8, 1 }, 1, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 1 },
        { TextureType::Tex2DArray, { 16, 8, 1 }, 3, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 3 },
        { TextureType::Tex3D, { 8, 8, 4 }, 1, D3D12_RESOURCE_DIMENSION_TEXTURE3D, 4 },
        { TextureType::Cube, { 8, 8, 1 }, 6, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 6 },
        { TextureType::CubeArray, { 8, 8, 1 }, 12, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 12 },
    };
    Device device = makeDevice();
    const MessageCollection messages;
    for (const TypeShape& shape : shapes)
    {
        INFO("texture type " << static_cast<int>(shape.type));
        const uint64_t allocationsBefore = device.counters().cpuAllocations;
        const Texture texture = makeTexture(
            device, { .type = shape.type, .format = Format::RGBA8Unorm, .extent = shape.extent, .layers = shape.layers, .usage = TextureUsage::Sampled }
        );
        // The payload is the one CPU allocation of a texture with no initial data and no target views.
        CHECK(device.counters().cpuAllocations - allocationsBefore == 1u);
        const D3D12_RESOURCE_DESC1 desc = descOf(payloadOf(device, texture).resource);
        CHECK(desc.Dimension == shape.dimension);
        CHECK(desc.DepthOrArraySize == shape.depthOrArraySize);
        CHECK(desc.MipLevels == UINT16{ 1 });
        // A view of the texture's own type takes that type's view dimension.
        Result<ViewIndex> view = device.createView(texture, { .type = shape.type });
        REQUIRE(view.has_value());
        device.destroy(*view);
        device.destroy(texture);
    }
    CHECK(messages.count() == 0u);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a texture's initial data is in every mip and layer when createTexture returns", "[d3d12]")
{
    constexpr uint32_t kMips = 3;
    constexpr uint32_t kLayers = 2;
    Device device = makeDevice();
    DeviceImpl& impl = DeviceImpl::of(device);
    // Entry mip + layer * mips, the reference's order.
    std::vector<std::vector<std::byte>> texels(kMips * kLayers);
    std::vector<TextureData> initial(kMips * kLayers);
    for (uint32_t layer = 0; layer < kLayers; ++layer)
    {
        for (uint32_t mip = 0; mip < kMips; ++mip)
        {
            const uint32_t index = mip + layer * kMips;
            const uint32_t width = std::max(16u >> mip, 1u);
            const uint32_t height = std::max(8u >> mip, 1u);
            texels[index] = pattern(size_t{ width } * height * 4, index * 37);
            initial[index] = TextureData{ .bytes = texels[index], .bytesPerRow = uint64_t{ width } * 4, .bytesPerImage = 0 };
        }
    }
    const Texture texture = makeTexture(
        device,
        { .type = TextureType::Tex2DArray,
          .format = Format::RGBA8Unorm,
          .extent = { 16, 8, 1 },
          .mips = kMips,
          .layers = kLayers,
          .usage = TextureUsage::Sampled },
        initial
    );
    ID3D12Resource* resource = payloadOf(device, texture).resource;
    for (uint32_t index = 0; index < kMips * kLayers; ++index)
    {
        INFO("subresource " << index);
        CHECK(test::readNativeTexture(impl, resource, index, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE) == texels[index]);
    }
    device.destroy(texture);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a texture with initial data is in the generic shader-resource layout when created", "[d3d12]")
{
    Device device = makeDevice();
    const std::vector<std::byte> texels = pattern(4 * 4 * 4, 5);
    const TextureData initial{ .bytes = texels, .bytesPerRow = 16, .bytesPerImage = 0 };
    const Texture texture =
        makeTexture(device, { .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled }, std::span(&initial, 1));
    const MessageCollection messages;
    // GPU-based validation checks the layout a barrier names against the one the resource is in.
    CHECK(test::readNativeTexture(DeviceImpl::of(device), payloadOf(device, texture).resource, 0, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE) == texels);
    CHECK(messages.count() == 0u);
    device.destroy(texture);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("an sRGB texture is castable or typeless", "[d3d12]")
{
    Device device = makeDevice();
    const bool relaxed = DeviceImpl::of(device).features.relaxedFormatCasting;
    const MessageCollection messages;
    const Texture texture = makeTexture(device, { .format = Format::RGBA8Unorm_sRGB, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled });
    const d3d12::TexturePayload& payload = payloadOf(device, texture);
    const DXGI_FORMAT expected = relaxed ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_TYPELESS;
    CHECK(payload.resourceFormat == expected);
    CHECK(descOf(payload.resource).Format == expected);
    Result<ViewIndex> linear = device.createView(texture, { .format = Format::RGBA8Unorm });
    REQUIRE(linear.has_value());
    Result<ViewIndex> own = device.createView(texture, {});
    REQUIRE(own.has_value());
    CHECK(messages.count() == 0u);
    device.destroy(*linear);
    device.destroy(*own);
    device.destroy(texture);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a sampled depth texture is typeless and its views read depth and stencil planes", "[d3d12]")
{
    Device device = makeDevice();
    const FormatCapabilities capabilities = device.formatCapabilities(Format::D32FloatS8);
    REQUIRE(capabilities.sampled);
    REQUIRE(capabilities.depthAttachment);
    const MessageCollection messages;
    const Texture sampled =
        makeTexture(device, { .format = Format::D32FloatS8, .extent = { 8, 8, 1 }, .usage = TextureUsage::Sampled | TextureUsage::DepthStencil });
    const d3d12::TexturePayload& payload = payloadOf(device, sampled);
    const D3D12_RESOURCE_DESC1 desc = descOf(payload.resource);
    CHECK(desc.Format == DXGI_FORMAT_R32G8X24_TYPELESS);
    CHECK((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0);
    CHECK((desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0);
    CHECK(payload.targetCount == 1u);
    Result<ViewIndex> depth = device.createView(sampled, { .aspect = TextureAspect::Depth });
    REQUIRE(depth.has_value());
    Result<ViewIndex> stencil = device.createView(sampled, { .aspect = TextureAspect::Stencil });
    REQUIRE(stencil.has_value());
    // A depth texture no shader samples is created as its depth format and denies shader views.
    const Texture attachment = makeTexture(device, { .format = Format::D32Float, .extent = { 8, 8, 1 }, .usage = TextureUsage::DepthStencil });
    const D3D12_RESOURCE_DESC1 attachmentDesc = descOf(payloadOf(device, attachment).resource);
    CHECK(attachmentDesc.Format == DXGI_FORMAT_D32_FLOAT);
    CHECK((attachmentDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0);
    // The stencil view reads plane 1 and returns the stencil value in its first component.
    core::TextureRecord record;
    record.format = Format::D32FloatS8;
    record.extent = { 8, 8, 1 };
    record.usage = TextureUsage::Sampled | TextureUsage::DepthStencil;
    const D3D12_SHADER_RESOURCE_VIEW_DESC stencilView = d3d12::shaderResourceDesc(record, { .aspect = TextureAspect::Stencil });
    CHECK(stencilView.Format == DXGI_FORMAT_X32_TYPELESS_G8X24_UINT);
    CHECK(stencilView.Texture2D.PlaneSlice == 1u);
    CHECK(
        stencilView.Shader4ComponentMapping == static_cast<UINT>(D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
                                                   D3D12_SHADER_COMPONENT_MAPPING_FROM_MEMORY_COMPONENT_1,
                                                   D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
                                                   D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
                                                   D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_1
                                               ))
    );
    const D3D12_SHADER_RESOURCE_VIEW_DESC depthView = d3d12::shaderResourceDesc(record, { .aspect = TextureAspect::Depth });
    CHECK(depthView.Texture2D.PlaneSlice == 0u);
    CHECK(depthView.Shader4ComponentMapping == static_cast<UINT>(D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING));
    CHECK(messages.count() == 0u);
    device.destroy(*depth);
    device.destroy(*stencil);
    device.destroy(sampled);
    device.destroy(attachment);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a render-target texture caches one RTV per mip and layer", "[d3d12]")
{
    Device device = makeDevice();
    const UINT increment = DeviceImpl::of(device).device()->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    const MessageCollection messages;

    const uint64_t beforeArray = device.counters().descriptorWrites;
    const Texture array = makeTexture(
        device,
        { .type = TextureType::Tex2DArray, .format = Format::RGBA8Unorm, .extent = { 16, 16, 1 }, .mips = 3, .layers = 4, .usage = TextureUsage::RenderTarget }
    );
    CHECK(device.counters().descriptorWrites - beforeArray == 12u);
    const d3d12::TexturePayload& arrayPayload = payloadOf(device, array);
    CHECK(arrayPayload.targetCount == 12u);
    REQUIRE(arrayPayload.targets.get() != nullptr);
    CHECK(arrayPayload.targets->GetDesc().Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    CHECK(arrayPayload.targets->GetDesc().NumDescriptors == 12u);
    const SIZE_T arrayStart = d3d12::targetView(arrayPayload, 0, 0).ptr;
    // Mip 2, layer 3 follows the four layers of each of mips 0 and 1.
    CHECK(d3d12::targetView(arrayPayload, 2, 3).ptr == arrayStart + SIZE_T{ 11 } * increment);

    const uint64_t beforeVolume = device.counters().descriptorWrites;
    const Texture volume = makeTexture(
        device, { .type = TextureType::Tex3D, .format = Format::RGBA8Unorm, .extent = { 8, 8, 4 }, .mips = 2, .usage = TextureUsage::RenderTarget }
    );
    // Four depth slices of mip 0 and two of mip 1.
    CHECK(device.counters().descriptorWrites - beforeVolume == 6u);
    const d3d12::TexturePayload& volumePayload = payloadOf(device, volume);
    CHECK(volumePayload.targetCount == 6u);
    CHECK(d3d12::targetView(volumePayload, 1, 1).ptr == d3d12::targetView(volumePayload, 0, 0).ptr + SIZE_T{ 5 } * increment);

    CHECK(messages.count() == 0u);
    device.destroy(volume);

    // A render-target texture's retirement releases its RTV heap too, one more native call than a sampled texture's.
    const uint32_t retireFrames = DeviceImpl::of(device).core.framesInFlight() + 1;
    const Texture plain = makeTexture(device, { .format = Format::RGBA8Unorm, .extent = { 16, 16, 1 }, .usage = TextureUsage::Sampled });
    emptyFrames(device, retireFrames);
    const uint64_t beforePlain = device.counters().nativeCreateCalls;
    device.destroy(plain);
    emptyFrames(device, retireFrames);
    const uint64_t plainRelease = device.counters().nativeCreateCalls - beforePlain;
    const uint64_t beforeTarget = device.counters().nativeCreateCalls;
    device.destroy(array);
    emptyFrames(device, retireFrames);
    CHECK(device.counters().nativeCreateCalls - beforeTarget == plainRelease + 1u);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a single-layer view of layer 0 takes the non-array dimension and a later layer the array one", "[d3d12]")
{
    core::TextureRecord array;
    array.type = TextureType::Tex2DArray;
    array.format = Format::RGBA8Unorm;
    array.extent = { 8, 8, 1 };
    array.layers = 4;
    array.usage = TextureUsage::Sampled | TextureUsage::Storage;
    const D3D12_SHADER_RESOURCE_VIEW_DESC first = d3d12::shaderResourceDesc(array, { .range = { .baseLayer = 0, .layerCount = 1 } });
    CHECK(first.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D);
    const D3D12_SHADER_RESOURCE_VIEW_DESC third = d3d12::shaderResourceDesc(array, { .range = { .baseLayer = 2, .layerCount = 1 } });
    CHECK(third.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY);
    CHECK(third.Texture2DArray.FirstArraySlice == 2u);
    CHECK(third.Texture2DArray.ArraySize == 1u);
    CHECK(
        d3d12::unorderedAccessDesc(array, { .range = { .baseLayer = 0, .layerCount = 1 }, .usage = TextureUsage::Storage }).ViewDimension ==
        D3D12_UAV_DIMENSION_TEXTURE2D
    );
    CHECK(
        d3d12::unorderedAccessDesc(array, { .range = { .baseLayer = 2, .layerCount = 1 }, .usage = TextureUsage::Storage }).ViewDimension ==
        D3D12_UAV_DIMENSION_TEXTURE2DARRAY
    );

    core::TextureRecord lines = array;
    lines.type = TextureType::Tex1DArray;
    lines.extent = { 8, 1, 1 };
    CHECK(d3d12::shaderResourceDesc(lines, { .type = TextureType::Tex1D, .range = { .layerCount = 1 } }).ViewDimension == D3D12_SRV_DIMENSION_TEXTURE1D);
    CHECK(
        d3d12::shaderResourceDesc(lines, { .type = TextureType::Tex1D, .range = { .baseLayer = 1, .layerCount = 1 } }).ViewDimension ==
        D3D12_SRV_DIMENSION_TEXTURE1DARRAY
    );

    core::TextureRecord multisampled = array;
    multisampled.samples = 4;
    multisampled.usage = TextureUsage::Sampled | TextureUsage::RenderTarget;
    CHECK(d3d12::shaderResourceDesc(multisampled, { .range = { .layerCount = 1 } }).ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DMS);
    CHECK(d3d12::shaderResourceDesc(multisampled, { .range = { .baseLayer = 1, .layerCount = 1 } }).ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY);

    core::TextureRecord cubes = array;
    cubes.type = TextureType::CubeArray;
    cubes.layers = 12;
    CHECK(d3d12::shaderResourceDesc(cubes, { .type = TextureType::Cube, .range = { .layerCount = 6 } }).ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBE);
    const D3D12_SHADER_RESOURCE_VIEW_DESC second =
        d3d12::shaderResourceDesc(cubes, { .type = TextureType::Cube, .range = { .baseLayer = 6, .layerCount = 6 } });
    CHECK(second.ViewDimension == D3D12_SRV_DIMENSION_TEXTURECUBEARRAY);
    CHECK(second.TextureCubeArray.First2DArrayFace == 6u);
    CHECK(second.TextureCubeArray.NumCubes == 1u);

    // The runtime accepts each form on real resources.
    Device device = makeDevice();
    const MessageCollection messages;
    const Texture texture =
        makeTexture(device, { .type = TextureType::Tex2DArray, .format = Format::RGBA8Unorm, .extent = { 8, 8, 1 }, .layers = 4, .usage = array.usage });
    std::vector<ViewIndex> views;
    for (const uint32_t layer : { 0u, 2u })
    {
        for (const TextureUsage usage : { TextureUsage::Sampled, TextureUsage::Storage })
        {
            Result<ViewIndex> view = device.createView(texture, { .range = { .baseLayer = layer, .layerCount = 1 }, .usage = usage });
            REQUIRE(view.has_value());
            views.push_back(*view);
        }
    }
    std::vector<Texture> textures = { texture };
    if ((device.formatCapabilities(Format::RGBA8Unorm).sampleCounts & 4u) != 0)
    {
        const Texture samples = makeTexture(
            device,
            { .type = TextureType::Tex2DArray, .format = Format::RGBA8Unorm, .extent = { 8, 8, 1 }, .layers = 2, .samples = 4, .usage = multisampled.usage }
        );
        textures.push_back(samples);
        for (const uint32_t layer : { 0u, 1u })
        {
            Result<ViewIndex> view = device.createView(samples, { .range = { .baseLayer = layer, .layerCount = 1 } });
            REQUIRE(view.has_value());
            views.push_back(*view);
        }
    }
    CHECK(messages.count() == 0u);
    for (const ViewIndex view : views)
    {
        device.destroy(view);
    }
    for (const Texture made : textures)
    {
        device.destroy(made);
    }
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a view holds its texture's resource until the view retires", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Texture texture = makeTexture(device, { .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled });
    ID3D12Resource* resource = payloadOf(device, texture).resource;
    const ULONG before = referencesOf(resource);
    Result<ViewIndex> view = device.createView(texture, {});
    REQUIRE(view.has_value());
    const ULONG held = referencesOf(resource);
    CHECK(held > before);
    // The slot keeps its reference after destroy until the frame the view was destroyed in retires. Destroying the texture
    // first would break VU-rojo-texture-0028, so the view's own reference is read on the live texture.
    device.destroy(*view);
    CHECK(referencesOf(resource) == held);
    emptyFrames(device, DeviceImpl::of(device).core.framesInFlight() + 1);
    CHECK(referencesOf(resource) < held);
    CHECK(messages.count() == 0u);
    device.destroy(texture);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a sampler is written at its sampler-heap index", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const uint64_t before = device.counters().descriptorWrites;
    Result<SamplerIndex> sampler = device.createSampler({});
    REQUIRE(sampler.has_value());
    CHECK(device.counters().descriptorWrites - before == 1u);
    CHECK(std::to_underlying(*sampler) < DeviceImpl::of(device).descriptors.samplerCapacity());
    CHECK(std::to_underlying(*sampler) < 2'048u);
    // The comparison and anisotropic filters encode as D3D12 filters the runtime accepts.
    Result<SamplerIndex> shadow =
        device.createSampler({ .address = SamplerAddress::ClampToEdge, .maxAnisotropy = 8, .compareEnable = true, .compare = CompareOp::GreaterOrEqual });
    REQUIRE(shadow.has_value());
    CHECK(*shadow != *sampler);
    CHECK(messages.count() == 0u);
    device.destroy(*sampler);
    device.destroy(*shadow);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("views of a reserved range are written at the range's indices", "[d3d12]")
{
    Device device = makeDevice();
    const MessageCollection messages;
    const Texture texture =
        makeTexture(device, { .format = Format::RGBA8Unorm, .extent = { 4, 4, 1 }, .usage = TextureUsage::Sampled | TextureUsage::Storage });
    Result<IndexRange> range = device.reserveViews(3);
    REQUIRE(range.has_value());
    const uint64_t before = device.counters().descriptorWrites;
    device.writeView(*range, 0, texture, {});
    device.writeView(*range, 2, texture, { .usage = TextureUsage::Storage });
    // A second write to a slot replaces the first.
    device.writeView(*range, 1, texture, {});
    device.writeView(*range, 1, texture, {});
    CHECK(device.counters().descriptorWrites - before == 4u);
    core::DeviceCore& core = DeviceImpl::of(device).core;
    for (uint32_t index = 0; index < 3; ++index)
    {
        CHECK(core.views().state(range->base + index) == core::SlotState::Assigned);
    }
    CHECK(messages.count() == 0u);
    device.release(*range);
    device.destroy(texture);
    destroyDevice(device);
}
