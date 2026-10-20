//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4ClearTests.cpp
/// @brief Tests how Metal 4 clears textures: render encoders with clear load actions, the clear kernels and the barriers around them.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/Counters.h"
#include "Metal4Barriers.h"
#include "Metal4Common.h"
#include "Metal4Device.h"
#include "Metal4Encoders.h"
#include "Metal4Kernels.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <Foundation/Foundation.hpp>
#include <Metal/Metal.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <span>
#include <string>

using namespace rojoRHI;

namespace {
    //==================================================================================================================
    /// The encoders of `list`.
    const metal4::Encoders& encodersOf(CommandList list)
    {
        return CommandListImpl::of(list).encoders;
    }

    //==================================================================================================================
    /// What `list` has counted so far.
    const core::RecordCounts& countsOf(CommandList list)
    {
        return core::coreOf(list).counts;
    }

    //==================================================================================================================
    /// A global barrier from copy writes to copy reads and writes; clears are copy writes.
    GlobalBarrier copyToCopy()
    {
        return GlobalBarrier{
            .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::Copy, .dstAccess = Access::CopyRead | Access::CopyWrite
        };
    }

    //==================================================================================================================
    /// Submits the ended `lists` to the queue of `type`, waits for them on the host and ends the frame.
    void submitAndWait(Device device, std::span<const CommandList> lists, QueueType type = QueueType::Graphics)
    {
        const Fence fence = *device.createFence(0, "test.done");
        const std::array<FenceValue, 1> signal{ FenceValue{ fence, 1 } };
        device.queue(type).submit(lists, {}, signal);
        CHECK(device.wait({ fence, 1 }, UINT64_MAX));
        device.endFrame();
    }

    //==================================================================================================================
    /// Creates a texture of `type` and `format` with `usage`, `extent`, `mips`, `layers` and `samples`.
    Texture makeTexture(
        Device device, TextureType type, Format format, TextureUsage usage, Extent3D extent, uint32_t mips = 1, uint32_t layers = 1, uint32_t samples = 1
    )
    {
        return *device.createTexture(
            TextureDesc{
                .type = type, .format = format, .extent = extent, .mips = mips, .layers = layers, .samples = samples, .usage = usage, .label = "test.texture" }
        );
    }
} // namespace

//======================================================================================================================
TEST_CASE("the render encoder has every stage the lowering assumes", "[metal4]")
{
    CHECK(metal4::kRenderStages == (MTL::StageVertex | MTL::StageObject | MTL::StageMesh | MTL::StageFragment));
    CHECK((metal4::kRenderStages & metal4::kComputeStages) == 0);
    // A clear by load action is copy work in the fragment stage.
    CHECK((metal4::metalStages(Stage::Copy) & metal4::kRenderStages) == MTL::StageFragment);
    Device device = *createDevice();
    const Texture target = makeTexture(device, TextureType::Tex2D, Format::RGBA8Unorm, TextureUsage::RenderTarget, { 8, 8, 1 });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.barrier(
        std::array{ GlobalBarrier{ .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::All, .dstAccess = Access::CopyWrite } }
    );
    const metal4::BarrierState& barriers = encodersOf(list).barriers();
    REQUIRE(barriers.pendingDestination == (metal4::kComputeStages | metal4::kRenderStages));
    list.clearTexture(target, {}, {});
    // The render encoder's opening barrier holds the fragment stage, where its load and store actions run; the other render
    // stages stay owed to the next encoder that runs them.
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(barriers.pendingDestination == (metal4::kComputeStages | (metal4::kRenderStages & ~MTL::StageFragment)));
    list.end();
    // The trailing encoder emits the rest.
    CHECK(countsOf(list).nativeBarriers == 3);
    submitAndWait(device, std::array{ list });
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a clear of many subresources emits a pending barrier's render part once", "[metal4]")
{
    Device device = *createDevice();
    const Texture target = makeTexture(device, TextureType::Tex2DArray, Format::RGBA8Unorm, TextureUsage::RenderTarget, { 8, 8, 1 }, 2, 2);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.barrier(
        std::array{ GlobalBarrier{ .srcStage = Stage::Copy, .srcAccess = Access::CopyWrite, .dstStage = Stage::All, .dstAccess = Access::CopyWrite } }
    );
    // Four render encoders: the first emits the fragment part; the vertex, object and mesh parts stay owed, not re-emitted by
    // each encoder.
    list.clearTexture(target, {}, ClearValue{});
    CHECK(countsOf(list).encodersOpened == 4);
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(encodersOf(list).barriers().pendingDestination == (metal4::kComputeStages | (metal4::kRenderStages & ~MTL::StageFragment)));
    list.end();
    submitAndWait(device, std::array{ list });
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a render-target clear opens one render encoder per subresource and closes the compute encoder", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .label = "test.b" });
    const Texture array = makeTexture(device, TextureType::Tex2DArray, Format::RGBA8Unorm, TextureUsage::RenderTarget, { 16, 16, 1 }, 3, 4);
    const Texture volume = makeTexture(device, TextureType::Tex3D, Format::RGBA8Unorm, TextureUsage::RenderTarget, { 8, 8, 8 }, 2);
    const Texture cube = makeTexture(device, TextureType::Cube, Format::RGBA16Float, TextureUsage::RenderTarget, { 8, 8, 1 }, 1, 6);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.copyBuffer(a, 0, b, 0, 256);
    REQUIRE(countsOf(list).nativeRecordCalls == 2 + 3 + 1);
    REQUIRE(encodersOf(list).kind() == metal4::EncoderKind::Compute);
    // Mips 1 and 2 of layers 1 and 2: four subresources. The compute encoder ends first; each render encoder counts its factory,
    // setLabel and endEncoding.
    list.clearTexture(array, SubresourceRange{ .baseMip = 1, .mipCount = kAllMips, .baseLayer = 1, .layerCount = 2 }, ClearValue{});
    CHECK(countsOf(list).nativeRecordCalls == 6 + 1 + 4 * 3);
    CHECK(countsOf(list).encodersOpened == 1 + 4);
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::None);
    CHECK_FALSE(encodersOf(list).holdsWork());
    // A 3D texture clears each depth plane of each mip: eight planes of mip 0 and four of mip 1.
    list.clearTexture(volume, {}, ClearValue{});
    CHECK(countsOf(list).nativeRecordCalls == 19 + 12 * 3);
    CHECK(countsOf(list).encodersOpened == 5 + 12);
    // A cube's layers are its faces.
    list.clearTexture(cube, SubresourceRange{ .baseLayer = 2, .layerCount = 3 }, ClearValue{});
    CHECK(countsOf(list).encodersOpened == 17 + 3);
    CHECK(countsOf(list).nativeBarriers == 0);
    list.end();
    submitAndWait(device, std::array{ list });
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a Storage-only clear dispatches one kernel per mip on the compute encoder", "[metal4]")
{
    Device device = *createDevice();
    const metal4::Kernels& kernels = DeviceImpl::of(device).kernels;
    const Texture array = makeTexture(device, TextureType::Tex2DArray, Format::RGBA8Unorm, TextureUsage::Storage, { 16, 16, 1 }, 3, 2);
    const Texture volume = makeTexture(device, TextureType::Tex3D, Format::R32Float, TextureUsage::Storage, { 8, 8, 4 }, 2);
    const QueueType type = GENERATE(QueueType::Graphics, QueueType::Compute);
    CAPTURE(static_cast<int>(type));
    device.beginFrame();
    CommandList list = device.acquireCommandList(type, "test.list");
    list.clearTexture(array, {}, ClearValue{ .color = { 0.25f, 0.5f, 0.75f, 1.0f } });
    // The encoder opens (3), on a graphics list waits on the carry fence before its first dispatch (1), and the texture is
    // bound once (1); the first mip sets the pipeline, the kernel table, the parameters, dispatches and sets the list's
    // table back (5); each later mip skips the pipeline (4).
    const uint64_t wait = type == QueueType::Graphics ? 1 : 0;
    CHECK(countsOf(list).nativeRecordCalls == 2 + 3 + wait + 1 + 5 + 4 + 4);
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(encodersOf(list).kind() == metal4::EncoderKind::Compute);
    CHECK(encodersOf(list).holdsWork());
    CHECK(encodersOf(list).kernelPipeline() == kernels.clear[static_cast<size_t>(metal4::ClearKernel::Tex2DArray)]);
    // Another texture type takes another pipeline, on the same encoder.
    list.clearTexture(volume, SubresourceRange{ .baseMip = 1 }, ClearValue{ .color = { 2.0f, 0.0f, 0.0f, 0.0f } });
    CHECK(countsOf(list).nativeRecordCalls == 19 + wait + 1 + 5);
    CHECK(countsOf(list).encodersOpened == 1);
    CHECK(encodersOf(list).kernelPipeline() == kernels.clear[static_cast<size_t>(metal4::ClearKernel::Tex3D)]);
    list.end();
    submitAndWait(device, std::array{ list }, type);
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a clear's render encoder emits the render part of a pending barrier and leaves none at end", "[metal4]")
{
    Device device = *createDevice();
    const Buffer a = *device.createBuffer({ .size = 256, .label = "test.a" });
    const Buffer b = *device.createBuffer({ .size = 256, .usage = BufferUsage::ShaderWrite, .label = "test.b" });
    const Texture target = makeTexture(device, TextureType::Tex2D, Format::RGBA8Unorm, TextureUsage::RenderTarget, { 8, 8, 1 });
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.copyBuffer(a, 0, b, 0, 256);
    list.barrier(std::array{ copyToCopy() });
    const metal4::BarrierState& barriers = encodersOf(list).barriers();
    REQUIRE(barriers.pendingDestination == (MTL::StageBlit | MTL::StageDispatch | MTL::StageFragment));
    REQUIRE(countsOf(list).nativeBarriers == 0);
    list.clearTexture(target, {}, ClearValue{ .color = { 1.0f, 0.0f, 0.0f, 1.0f } });
    // The render encoder emits the fragment part; the blit and dispatch parts wait for the next compute encoder.
    CHECK(countsOf(list).nativeBarriers == 1);
    CHECK(countsOf(list).encodersOpened == 2);
    CHECK(barriers.pendingDestination == metal4::kComputeStages);
    // A blit and a dispatch on the next compute encoder, whose opening barrier emits the rest.
    list.fillBuffer(b, 0, 64, 0x55555555u);
    list.fillBuffer(b, 64, 64, 0x01020304u);
    CHECK(countsOf(list).nativeBarriers == 2);
    CHECK(barriers.pendingDestination == 0);
    list.end();
    // Nothing was owed at the end, so no trailing encoder opened.
    CHECK(countsOf(list).encodersOpened == 3);
    CHECK(countsOf(list).nativeBarriers == 2);
    submitAndWait(device, std::array{ list });
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("a D32FloatS8 clear runs under the validation layer with depth and stencil attachments", "[metal4]")
{
    Device device = *createDevice();
    const Texture depthStencil = makeTexture(device, TextureType::Tex2DArray, Format::D32FloatS8, TextureUsage::DepthStencil, { 16, 16, 1 }, 2, 2);
    const Texture depth = makeTexture(device, TextureType::Cube, Format::D32Float, TextureUsage::DepthStencil, { 8, 8, 1 }, 1, 6);
    const Texture multisample = makeTexture(device, TextureType::Tex2DArray, Format::RGBA8Unorm, TextureUsage::RenderTarget, { 8, 8, 1 }, 1, 2, 4);
    device.beginFrame();
    CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
    list.clearTexture(depthStencil, {}, ClearValue{ .depth = 0.5f, .stencil = 7 });
    CHECK(countsOf(list).encodersOpened == 4);
    list.clearTexture(depth, SubresourceRange{ .baseLayer = 4 }, ClearValue{ .depth = 1.0f });
    CHECK(countsOf(list).encodersOpened == 4 + 2);
    // Every sample of a multisample layer clears by its load action.
    list.clearTexture(multisample, {}, ClearValue{ .color = { 0.0f, 1.0f, 0.0f, 1.0f } });
    CHECK(countsOf(list).encodersOpened == 6 + 2);
    list.end();
    submitAndWait(device, std::array{ list });
    destroyDevice(device);
}

//======================================================================================================================
TEST_CASE("the kernel set holds one clear pipeline per Storage texture type, from at most three functions", "[metal4]")
{
    Device device = *createDevice();
    const metal4::Kernels& kernels = DeviceImpl::of(device).kernels;
    constexpr std::array<const char*, metal4::kClearKernelCount> kLabels{
        "rojorhi.kernel.clear.1D", "rojorhi.kernel.clear.1DArray", "rojorhi.kernel.clear.2D",        "rojorhi.kernel.clear.2DArray",
        "rojorhi.kernel.clear.3D", "rojorhi.kernel.clear.Cube",    "rojorhi.kernel.clear.CubeArray",
    };
    std::set<const MTL::ComputePipelineState*> distinct{ kernels.fill };
    {
        // The label getters and the function names autorelease their results.
        const metal4::AutoreleasePool pool;
        for (size_t i = 0; i < metal4::kClearKernelCount; ++i)
        {
            CAPTURE(i);
            REQUIRE(kernels.clear[i] != nullptr);
            CHECK(metal4::toStdString(kernels.clear[i]->label()) == kLabels[i]);
            distinct.insert(kernels.clear[i]);
        }
        CHECK(distinct.size() == 1 + metal4::kClearKernelCount);
        const NS::Array* names = kernels.library->functionNames();
        size_t clearFunctions = 0;
        for (NS::UInteger i = 0; i < names->count(); ++i)
        {
            if (metal4::toStdString(names->object<NS::String>(i)).starts_with("rojorhi_clear"))
            {
                ++clearFunctions;
            }
        }
        CHECK(clearFunctions >= 1);
        CHECK(clearFunctions <= 3);
    }
    CHECK(metal4::clearKernelOf(MTL::TextureType1D) == metal4::ClearKernel::Tex1D);
    CHECK(metal4::clearKernelOf(MTL::TextureType1DArray) == metal4::ClearKernel::Tex1DArray);
    CHECK(metal4::clearKernelOf(MTL::TextureType2D) == metal4::ClearKernel::Tex2D);
    CHECK(metal4::clearKernelOf(MTL::TextureType2DArray) == metal4::ClearKernel::Tex2DArray);
    CHECK(metal4::clearKernelOf(MTL::TextureType3D) == metal4::ClearKernel::Tex3D);
    CHECK(metal4::clearKernelOf(MTL::TextureTypeCube) == metal4::ClearKernel::Cube);
    CHECK(metal4::clearKernelOf(MTL::TextureTypeCubeArray) == metal4::ClearKernel::CubeArray);
    destroyDevice(device);
}
