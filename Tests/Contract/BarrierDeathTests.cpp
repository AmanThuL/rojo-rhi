//----------------------------------------------------------------------------------------------------------------------
/// @file BarrierDeathTests.cpp
/// @brief Death tests for the valid-usage rules of barriers that need no resource tracking.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Death.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

using namespace rojoRHI;
using test::PassKit;

namespace {
    //==================================================================================================================
    GlobalBarrier global(Stage src, Access srcAccess, Stage dst, Access dstAccess)
    {
        return GlobalBarrier{ .srcStage = src, .srcAccess = srcAccess, .dstStage = dst, .dstAccess = dstAccess };
    }

    //==================================================================================================================
    void recordGlobal(PassKit& kit, const GlobalBarrier& barrier)
    {
        const std::array<GlobalBarrier, 1> globals{ barrier };
        kit.list.barrier(globals);
    }

    //==================================================================================================================
    void recordBuffer(PassKit& kit, const BufferBarrier& barrier)
    {
        const std::array<BufferBarrier, 1> buffers{ barrier };
        kit.list.barrier({}, buffers);
    }

    //==================================================================================================================
    /// Returns a barrier of `range` of `texture` that obeys every rule but the range rules.
    TextureBarrier rangeBarrier(Texture texture, const SubresourceRange& range)
    {
        return TextureBarrier{ .texture = texture,
                               .range = range,
                               .srcStage = Stage::ComputeShader,
                               .dstStage = Stage::ComputeShader,
                               .oldLayout = Layout::Undefined,
                               .newLayout = Layout::General };
    }

    //==================================================================================================================
    void recordTexture(PassKit& kit, const TextureBarrier& barrier)
    {
        const std::array<TextureBarrier, 1> textures{ barrier };
        kit.list.barrier({}, {}, textures);
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0001 a barrier on a Copy list names a compute stage", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0001", [] {
        PassKit kit(QueueType::Copy);
        recordGlobal(kit, global(Stage::ComputeShader, Access::None, Stage::Copy, Access::None));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0002 an access is not performed by the stages of its side", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0002", [] {
        PassKit kit;
        recordGlobal(kit, global(Stage::Copy, Access::ShaderWrite, Stage::ComputeShader, Access::None));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0003 a side with stage None names an access", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0003", [] {
        PassKit kit;
        recordGlobal(kit, global(Stage::None, Access::ShaderRead, Stage::ComputeShader, Access::None));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0007 a barrier on a Compute list uses a layout the queue lacks", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0007", [] {
        PassKit kit(QueueType::Compute);
        recordTexture(kit, TextureBarrier{ .texture = kit.texture(Format::RGBA8Unorm, TextureUsage::RenderTarget), .newLayout = Layout::ColorAttachment });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0026 a barrier on a Copy list moves a texture out of General", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0026", [] {
        PassKit kit(QueueType::Copy);
        recordTexture(kit, TextureBarrier{ .texture = kit.texture(), .oldLayout = Layout::General, .newLayout = Layout::CopyDst });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0026 a barrier on a Copy list discards a texture from Undefined", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0026", [] {
        PassKit kit(QueueType::Copy);
        recordTexture(kit, TextureBarrier{ .texture = kit.texture(), .oldLayout = Layout::Undefined, .newLayout = Layout::General });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0008 a global barrier has no stage on either side", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0008", [] {
        PassKit kit;
        recordGlobal(kit, global(Stage::None, Access::None, Stage::None, Access::None));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0010 a buffer barrier starts at the end of its buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0010", [] {
        PassKit kit;
        recordBuffer(kit, BufferBarrier{ .buffer = kit.buffer(BufferUsage::None, 256), .offset = 256, .size = 0 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0010 a buffer barrier range wraps past 64 bits", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0010", [] {
        PassKit kit;
        recordBuffer(kit, BufferBarrier{ .buffer = kit.buffer(BufferUsage::None, 256), .offset = 16, .size = kWholeSize - 8 });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0011 a buffer barrier names a texture access", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0011", [] {
        PassKit kit;
        recordBuffer(
            kit,
            BufferBarrier{ .buffer = kit.buffer(BufferUsage::None),
                           .srcStage = Stage::ColorAttachment,
                           .srcAccess = Access::ColorAttachmentWrite,
                           .dstStage = Stage::ComputeShader }
        );
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0014 a texture barrier moves to Undefined", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0014", [] {
        PassKit kit;
        recordTexture(kit, TextureBarrier{ .texture = kit.texture(), .oldLayout = Layout::General, .newLayout = Layout::Undefined });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0015 a texture barrier moves to a layout the usage lacks", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0015", [] {
        PassKit kit;
        recordTexture(kit, TextureBarrier{ .texture = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled), .newLayout = Layout::ShaderWrite });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0027 a texture barrier moves a depth texture without Sampled to DepthStencilRead", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0027", [] {
        PassKit kit;
        recordTexture(kit, TextureBarrier{ .texture = kit.texture(Format::D32Float, TextureUsage::DepthStencil), .newLayout = Layout::DepthStencilRead });
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0016 an access is not permitted by the layout it follows", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0016", [] {
        PassKit kit;
        recordTexture(
            kit,
            TextureBarrier{ .texture = kit.texture(),
                            .srcStage = Stage::ComputeShader,
                            .srcAccess = Access::ShaderRead,
                            .dstStage = Stage::ComputeShader,
                            .oldLayout = Layout::Undefined,
                            .newLayout = Layout::General }
        );
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0019 a barrier is recorded inside a render pass", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0019", [] {
        PassKit kit;
        kit.beginSimplePass();
        recordGlobal(kit, global(Stage::Copy, Access::None, Stage::Copy, Access::None));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0020 two texture barriers of a batch cover one subresource", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0020", [] {
        PassKit kit;
        const Texture texture = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled, 4, 1);
        const std::array<TextureBarrier, 2> textures{
            TextureBarrier{ .texture = texture, .range = { .baseMip = 0, .mipCount = 2 }, .newLayout = Layout::General },
            TextureBarrier{ .texture = texture, .range = { .baseMip = 1, .mipCount = kAllMips }, .newLayout = Layout::General }
        };
        kit.list.barrier({}, {}, textures);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0001 a barrier on a Compute list names a render stage", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0001", [] {
        PassKit kit(QueueType::Compute);
        recordGlobal(kit, global(Stage::ColorAttachment, Access::None, Stage::ComputeShader, Access::None));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0016 a destination access is not permitted by the new layout", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0016", [] {
        PassKit kit;
        recordTexture(
            kit,
            TextureBarrier{ .texture = kit.texture(),
                            .srcStage = Stage::ComputeShader,
                            .dstStage = Stage::ComputeShader,
                            .dstAccess = Access::ShaderWrite,
                            .oldLayout = Layout::General,
                            .newLayout = Layout::ShaderRead }
        );
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0016 a texture barrier range has no mips", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0016", [] {
        PassKit kit;
        recordTexture(kit, rangeBarrier(kit.texture(), { .mipCount = 0 }));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0017 a texture barrier range ends past the last mip", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0017", [] {
        PassKit kit;
        recordTexture(kit, rangeBarrier(kit.texture(), { .baseMip = 1, .mipCount = 1 }));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-texture-0041 a texture barrier range with all counts starts past the last mip", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-texture-0041", [] {
        PassKit kit;
        recordTexture(kit, rangeBarrier(kit.texture(), { .baseMip = 1 }));
    });
}
