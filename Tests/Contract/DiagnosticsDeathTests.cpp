//----------------------------------------------------------------------------------------------------------------------
/// @file DiagnosticsDeathTests.cpp
/// @brief Death tests for the valid-usage rules of access declarations and capture in chapter 14 that Null can reach.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Death.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace rojoRHI;
using rojoRHI::test::PassKit;

namespace {
    /// A device with validation, a recording list, and the resources the valid declarations below name.
    struct Rig
    {
        PassKit kit{ QueueType::Graphics, true };
        Buffer buffer = kit.buffer(BufferUsage::ShaderWrite);                                                   // 4096 bytes
        Texture texture = kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled | TextureUsage::Storage, 2, 2); // 2 mips, 2 layers
    };

    //==================================================================================================================
    /// Returns a valid fragment read of the rig's buffer.
    ResourceAccess valid(const Rig& rig)
    {
        return ResourceAccess{ .buffer = rig.buffer, .stage = Stage::FragmentShader, .access = Access::ShaderRead };
    }

    //==================================================================================================================
    /// Returns a valid fragment read of the rig's texture.
    ResourceAccess validTexture(const Rig& rig)
    {
        return ResourceAccess{ .texture = rig.texture, .stage = Stage::FragmentShader, .access = Access::ShaderRead };
    }

    //==================================================================================================================
    /// Declares `access` on the rig's list.
    void declare(Rig& rig, const ResourceAccess& access)
    {
        const std::array<ResourceAccess, 1> accesses{ access };
        rig.kit.list.declareAccess(accesses);
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0001 neither a buffer nor a texture is declared", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0001", [] {
        Rig rig;
        ResourceAccess access = valid(rig);
        access.buffer = {};
        declare(rig, access);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0001 both a buffer and a texture are declared", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0001", [] {
        Rig rig;
        ResourceAccess access = valid(rig);
        access.texture = rig.texture;
        declare(rig, access);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0002 the buffer offset is past the end", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0002", [] {
        Rig rig;
        ResourceAccess access = valid(rig);
        access.offset = 8192;
        access.size = 4;
        declare(rig, access);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0002 the buffer range runs past the end", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0002", [] {
        Rig rig;
        ResourceAccess access = valid(rig);
        access.offset = 4000;
        access.size = 200;
        declare(rig, access);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0002 the texture range starts past the last mip", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0002", [] {
        Rig rig;
        ResourceAccess access = validTexture(rig);
        access.range = SubresourceRange{ .baseMip = 2, .mipCount = 1 };
        declare(rig, access);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0002 the texture range runs past the last layer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0002", [] {
        Rig rig;
        ResourceAccess access = validTexture(rig);
        access.range = SubresourceRange{ .baseLayer = 1, .layerCount = 2 };
        declare(rig, access);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0002 the texture range has no mips", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0002", [] {
        Rig rig;
        ResourceAccess access = validTexture(rig);
        access.range = SubresourceRange{ .mipCount = 0 };
        declare(rig, access);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0002 the texture range has no layers", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0002", [] {
        Rig rig;
        ResourceAccess access = validTexture(rig);
        access.range = SubresourceRange{ .layerCount = 0 };
        declare(rig, access);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0003 the stage is not a shader stage", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0003", [] {
        Rig rig;
        ResourceAccess access = valid(rig);
        access.stage = Stage::Copy;
        declare(rig, access);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0003 the stage names two shader stages", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0003", [] {
        Rig rig;
        ResourceAccess access = valid(rig);
        access.stage = Stage::VertexShader | Stage::FragmentShader;
        declare(rig, access);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0003 the stage is empty", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0003", [] {
        Rig rig;
        ResourceAccess access = valid(rig);
        access.stage = Stage::None;
        declare(rig, access);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0003 the access holds a bit other than a shader access", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0003", [] {
        Rig rig;
        ResourceAccess access = valid(rig);
        access.access = Access::ShaderRead | Access::CopyRead;
        declare(rig, access);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0003 the access is empty", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0003", [] {
        Rig rig;
        ResourceAccess access = valid(rig);
        access.access = Access::None;
        declare(rig, access);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0004 an access is declared on a list that has ended", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0004", [] {
        Rig rig;
        rig.kit.list.end();
        declare(rig, valid(rig));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0005 a buffer write is declared on a buffer without ShaderWrite", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0005", [] {
        Rig rig;
        ResourceAccess access = valid(rig);
        access.buffer = rig.kit.buffer(BufferUsage::IndirectArgs);
        access.access = Access::ShaderWrite;
        declare(rig, access);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0005 a texture read is declared without Sampled or Storage", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0005", [] {
        Rig rig;
        ResourceAccess access = validTexture(rig);
        access.texture = rig.kit.texture(Format::RGBA8Unorm, TextureUsage::RenderTarget);
        declare(rig, access);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0005 a texture write is declared on a texture without Storage", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0005", [] {
        Rig rig;
        ResourceAccess access = validTexture(rig);
        access.texture = rig.kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled);
        access.access = Access::ShaderWrite;
        declare(rig, access);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0005 a texture read and write are declared without Storage", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0005", [] {
        Rig rig;
        ResourceAccess access = validTexture(rig);
        access.texture = rig.kit.texture(Format::RGBA8Unorm, TextureUsage::Sampled);
        access.access = Access::ShaderRead | Access::ShaderWrite;
        declare(rig, access);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-sync-0024 a Storage-only texture read lacks its producer's destination access", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-sync-0024", [] {
        PassKit kit(QueueType::Graphics, true);
        const Texture texture = kit.texture(Format::RGBA8Unorm, TextureUsage::Storage);
        test::transition(kit.list, texture, Layout::Undefined, Layout::CopyDst);
        kit.list.clearTexture(texture, {}, ClearValue{ .color = { 1, 0, 1, 1 } });
        const std::array barriers{ TextureBarrier{ .texture = texture,
                                                   .srcStage = Stage::Copy,
                                                   .srcAccess = Access::CopyWrite,
                                                   .dstStage = Stage::ComputeShader,
                                                   .dstAccess = Access::ShaderWrite,
                                                   .oldLayout = Layout::CopyDst,
                                                   .newLayout = Layout::ShaderWrite } };
        kit.list.barrier({}, {}, barriers);
        kit.list.setPipeline(kit.compute());
        const std::array accesses{ ResourceAccess{ .texture = texture, .stage = Stage::ComputeShader, .access = Access::ShaderRead } };
        kit.list.declareAccess(accesses);
        kit.list.dispatch(1, 1, 1);
        kit.finish();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0006 a compute stage is declared for a draw", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0006", [] {
        Rig rig;
        rig.kit.readyToDraw();
        ResourceAccess access = valid(rig);
        access.stage = Stage::ComputeShader;
        declare(rig, access);
        rig.kit.list.draw(3);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0006 a graphics stage is declared for a dispatch", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0006", [] {
        Rig rig;
        rig.kit.list.setPipeline(rig.kit.compute());
        declare(rig, valid(rig));
        rig.kit.list.dispatch(1, 1, 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0007 a list ends with a declaration no draw followed", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0007", [] {
        Rig rig;
        declare(rig, valid(rig));
        rig.kit.list.end();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0009 the capture path is empty", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0009", [] {
        Device device = *createDevice();
        (void)device.beginCapture("");
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-diag-0010 endCapture is called with no capture in progress", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-diag-0010", [] {
        Device device = *createDevice();
        device.endCapture();
    });
}
