//----------------------------------------------------------------------------------------------------------------------
/// @file DrawDeathTests.cpp
/// @brief Death tests for the valid-usage rules of roots, draws, dispatches, indirect commands and the addresses they take.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Death.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

using namespace rojoRHI;
using test::PassKit;

namespace {
    constexpr std::array<Format, 1> kRgba8 = { Format::RGBA8Unorm };
    constexpr std::array<std::byte, 16> kBytes = {};

    //==================================================================================================================
    /// Opens a pass with a vertex pipeline bound and returns an address 256 bytes into an `IndirectArgs` buffer.
    Addr readyIndirect(PassKit& kit, bool mesh = false)
    {
        kit.readyToDraw(mesh);
        return kit.device.address(kit.buffer(BufferUsage::IndirectArgs), 256);
    }

    //==================================================================================================================
    /// Returns an address into a buffer without the `IndirectArgs` usage.
    Addr plainAddress(PassKit& kit)
    {
        return kit.device.address(kit.buffer(BufferUsage::None), 256);
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-abi-0001 setRoot is given the null address", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-abi-0001", [] {
        PassKit kit;
        kit.list.setRoot(Addr{});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-abi-0002 setRoot is given an address not 256 bytes aligned", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-abi-0002", [] {
        PassKit kit;
        kit.list.setRoot(kit.device.address(kit.buffer(BufferUsage::None), 4));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0009 setRoot is given an address of a destroyed buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0009", [] {
        PassKit kit;
        const Buffer buffer = kit.buffer(BufferUsage::None);
        const Addr address = kit.device.address(buffer);
        kit.device.destroy(buffer);
        kit.list.setRoot(address);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0009 an indirect draw is given an address past the end of its buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0009", [] {
        PassKit kit;
        kit.readyToDraw();
        const Buffer buffer = kit.buffer(BufferUsage::IndirectArgs, 1024);
        kit.list.drawIndirect(kit.device.address(buffer, 1023) + 1, 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0009 a dispatch is given the address of a destroyed buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0009", [] {
        PassKit kit;
        kit.list.setPipeline(kit.compute());
        const Buffer buffer = kit.buffer(BufferUsage::IndirectArgs);
        const Addr address = kit.device.address(buffer);
        kit.device.destroy(buffer);
        kit.list.dispatchIndirect(address);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0020 setRoot is given frame data of an earlier frame", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0020", [] {
        PassKit kit;
        const Addr stale = kit.list.uploadFrameData(kBytes);
        kit.list.end();
        const std::array<CommandList, 1> lists{ kit.list };
        kit.device.queue(QueueType::Graphics).submit(lists);
        kit.device.endFrame();
        kit.device.beginFrame();
        CommandList next = kit.device.acquireCommandList(QueueType::Graphics, "next");
        next.setRoot(stale);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0020 an indirect dispatch reads frame data of an earlier frame", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0020", [] {
        PassKit kit;
        const Addr stale = kit.list.uploadFrameData(kBytes);
        kit.list.end();
        const std::array<CommandList, 1> lists{ kit.list };
        kit.device.queue(QueueType::Graphics).submit(lists);
        kit.device.endFrame();
        kit.device.beginFrame();
        CommandList next = kit.device.acquireCommandList(QueueType::Graphics, "next");
        next.setPipeline(kit.compute());
        next.dispatchIndirect(stale);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0023 draw is recorded outside a render pass", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0023", [] {
        PassKit kit;
        kit.list.draw(3);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0023 draw is recorded with a mesh pipeline bound", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0023", [] {
        PassKit kit;
        kit.readyToDraw(true);
        kit.list.draw(3);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0023 draw is recorded with no pipeline bound", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0023", [] {
        PassKit kit;
        kit.beginSimplePass();
        kit.list.draw(3);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0024 draw is recorded before a viewport and scissor are set", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0024", [] {
        PassKit kit;
        kit.beginSimplePass();
        kit.list.setPipeline(kit.graphics(kRgba8));
        kit.list.draw(3);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0025 drawIndexed is recorded before a viewport and scissor are set", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0025", [] {
        PassKit kit;
        kit.beginSimplePass();
        kit.list.setPipeline(kit.graphics(kRgba8));
        kit.list.drawIndexed(kit.buffer(BufferUsage::Index), IndexType::UInt16, 3);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0025 drawIndexed is recorded outside a render pass", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0025", [] {
        PassKit kit;
        kit.list.drawIndexed(kit.buffer(BufferUsage::Index), IndexType::UInt16, 3);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0026 drawIndexed reads a buffer without the Index usage", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0026", [] {
        PassKit kit;
        kit.readyToDraw();
        kit.list.drawIndexed(kit.buffer(BufferUsage::None), IndexType::UInt16, 3);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0026 drawIndexed reads indices past the end of the buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0026", [] {
        PassKit kit;
        kit.readyToDraw();
        kit.list.drawIndexed(kit.buffer(BufferUsage::Index, 64), IndexType::UInt32, 8, 1, 9);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0027 drawMeshTasks is recorded with a vertex pipeline bound", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0027", [] {
        PassKit kit;
        kit.readyToDraw();
        kit.list.drawMeshTasks(1, 1, 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0027 drawMeshTasks is recorded before a viewport and scissor are set", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0027", [] {
        PassKit kit;
        kit.beginSimplePass();
        kit.list.setPipeline(kit.graphics(kRgba8, Format::Undefined, 1, PrimitiveClass::Triangle, false, true));
        kit.list.drawMeshTasks(1, 1, 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0028 drawMeshTasks launches more groups than a dimension allows", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0028", [] {
        PassKit kit;
        kit.readyToDraw(true);
        kit.list.drawMeshTasks(1, 1, kit.device.capabilities().limits.maxMeshTaskGroups + 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0028 drawMeshTasks launches more groups in total than allowed", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0028", [] {
        PassKit kit;
        kit.readyToDraw(true);
        const Limits limits = kit.device.capabilities().limits;
        kit.list.drawMeshTasks(limits.maxMeshTaskGroups, limits.maxMeshTaskGroups, 2);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0029 dispatch is recorded inside a render pass", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0029", [] {
        PassKit kit;
        kit.readyToDraw();
        kit.list.dispatch(1, 1, 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0029 dispatch is recorded on a Copy list", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0029", [] {
        PassKit kit(QueueType::Copy);
        kit.list.dispatch(1, 1, 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0030 dispatch is recorded with no compute pipeline bound", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0030", [] {
        PassKit kit;
        kit.list.dispatch(1, 1, 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0030 dispatch is recorded after a render pass unbound the pipeline", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0030", [] {
        PassKit kit;
        kit.list.setPipeline(kit.compute());
        kit.beginSimplePass();
        kit.list.endRenderPass();
        kit.list.dispatch(1, 1, 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0031 dispatch launches more groups than a dimension allows", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0031", [] {
        PassKit kit;
        kit.list.setPipeline(kit.compute());
        kit.list.dispatch(1, kit.device.capabilities().limits.maxDispatchGroups + 1, 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0001 drawIndirect is recorded outside a render pass", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0001", [] {
        PassKit kit;
        kit.list.drawIndirect(kit.device.address(kit.buffer(BufferUsage::IndirectArgs)), 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0001 drawIndirect is recorded with a mesh pipeline bound", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0001", [] {
        PassKit kit;
        const Addr args = readyIndirect(kit, true);
        kit.list.drawIndirect(args, 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0002 drawIndirect is given the null address", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0002", [] {
        PassKit kit;
        kit.readyToDraw();
        kit.list.drawIndirect(Addr{}, 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0002 drawIndirect is given an address not 4 bytes aligned", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0002", [] {
        PassKit kit;
        const Addr args = readyIndirect(kit);
        kit.list.drawIndirect(args + 2, 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0002 drawIndirect is given a buffer without IndirectArgs", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0002", [] {
        PassKit kit;
        kit.readyToDraw();
        kit.list.drawIndirect(plainAddress(kit), 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0002 drawIndirect reads records past the end of the buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0002", [] {
        PassKit kit;
        const Addr args = readyIndirect(kit);
        kit.list.drawIndirect(args, 4096 / 20);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0003 drawIndirect is given a maxCount of zero", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0003", [] {
        PassKit kit;
        const Addr args = readyIndirect(kit);
        kit.list.drawIndirect(args, 0);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0003 drawIndirect is given a maxCount above the limit", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0003", [] {
        PassKit kit;
        kit.readyToDraw();
        const Buffer big = kit.buffer(BufferUsage::IndirectArgs, 20ull * 70000);
        kit.list.drawIndirect(kit.device.address(big), kit.device.capabilities().limits.maxDrawIndirectCount + 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0004 drawIndirect is given a count address not 4 bytes aligned", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0004", [] {
        PassKit kit;
        const Addr args = readyIndirect(kit);
        kit.list.drawIndirect(args, 1, args + 1026);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0004 drawIndirect is given a count in a buffer without IndirectArgs", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0004", [] {
        PassKit kit;
        const Addr args = readyIndirect(kit);
        kit.list.drawIndirect(args, 1, plainAddress(kit));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0006 drawIndexedIndirect is recorded outside a render pass", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0006", [] {
        PassKit kit;
        kit.list.drawIndexedIndirect(kit.buffer(BufferUsage::Index), IndexType::UInt16, kit.device.address(kit.buffer(BufferUsage::IndirectArgs)), 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0007 drawIndexedIndirect reads a buffer without the Index usage", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0007", [] {
        PassKit kit;
        const Addr args = readyIndirect(kit);
        kit.list.drawIndexedIndirect(kit.buffer(BufferUsage::None), IndexType::UInt16, args, 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0008 drawIndexedIndirect is given the null address", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0008", [] {
        PassKit kit;
        kit.readyToDraw();
        kit.list.drawIndexedIndirect(kit.buffer(BufferUsage::Index), IndexType::UInt16, Addr{}, 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0008 drawIndexedIndirect reads records past the end of the buffer", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0008", [] {
        PassKit kit;
        const Addr args = readyIndirect(kit);
        kit.list.drawIndexedIndirect(kit.buffer(BufferUsage::Index), IndexType::UInt16, args, 4096 / 24);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0009 drawIndexedIndirect is given a maxCount of zero", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0009", [] {
        PassKit kit;
        const Addr args = readyIndirect(kit);
        kit.list.drawIndexedIndirect(kit.buffer(BufferUsage::Index), IndexType::UInt16, args, 0);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0010 drawIndexedIndirect is given a count address not 4 bytes aligned", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0010", [] {
        PassKit kit;
        const Addr args = readyIndirect(kit);
        kit.list.drawIndexedIndirect(kit.buffer(BufferUsage::Index), IndexType::UInt16, args, 1, args + 1026);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0012 drawMeshTasksIndirect is recorded with a vertex pipeline bound", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0012", [] {
        PassKit kit;
        const Addr args = readyIndirect(kit);
        kit.list.drawMeshTasksIndirect(args, 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0014 drawMeshTasksIndirect is given the null address", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0014", [] {
        PassKit kit;
        kit.readyToDraw(true);
        kit.list.drawMeshTasksIndirect(Addr{}, 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0014 drawMeshTasksIndirect is given a buffer without IndirectArgs", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0014", [] {
        PassKit kit;
        kit.readyToDraw(true);
        kit.list.drawMeshTasksIndirect(plainAddress(kit), 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0015 drawMeshTasksIndirect is given a maxCount of zero", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0015", [] {
        PassKit kit;
        const Addr args = readyIndirect(kit, true);
        kit.list.drawMeshTasksIndirect(args, 0);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0016 drawMeshTasksIndirect is given a count in a buffer without IndirectArgs", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0016", [] {
        PassKit kit;
        const Addr args = readyIndirect(kit, true);
        kit.list.drawMeshTasksIndirect(args, 1, plainAddress(kit));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0018 dispatchIndirect is recorded with no compute pipeline bound", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0018", [] {
        PassKit kit;
        kit.list.dispatchIndirect(kit.device.address(kit.buffer(BufferUsage::IndirectArgs)));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0018 dispatchIndirect is recorded inside a render pass", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0018", [] {
        PassKit kit;
        const Addr args = readyIndirect(kit);
        kit.list.dispatchIndirect(args);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0019 dispatchIndirect is given the null address", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0019", [] {
        PassKit kit;
        kit.list.setPipeline(kit.compute());
        kit.list.dispatchIndirect(Addr{});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0019 dispatchIndirect is given a buffer without IndirectArgs", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0019", [] {
        PassKit kit;
        kit.list.setPipeline(kit.compute());
        kit.list.dispatchIndirect(plainAddress(kit));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0021 drawIndirect is recorded before a viewport and scissor are set", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0021", [] {
        PassKit kit;
        const Addr args = kit.device.address(kit.buffer(BufferUsage::IndirectArgs), 256);
        kit.beginSimplePass();
        kit.list.setPipeline(kit.graphics(kRgba8));
        kit.list.drawIndirect(args, 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0022 drawIndexedIndirect is recorded before a viewport and scissor are set", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0022", [] {
        PassKit kit;
        const Addr args = kit.device.address(kit.buffer(BufferUsage::IndirectArgs), 256);
        const Buffer indices = kit.buffer(BufferUsage::Index);
        kit.beginSimplePass();
        kit.list.setPipeline(kit.graphics(kRgba8));
        kit.list.drawIndexedIndirect(indices, IndexType::UInt16, args, 1);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-indirect-0023 drawMeshTasksIndirect is recorded before a viewport and scissor are set", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-indirect-0023", [] {
        PassKit kit;
        const Addr args = kit.device.address(kit.buffer(BufferUsage::IndirectArgs), 256);
        kit.beginSimplePass();
        kit.list.setPipeline(kit.graphics(kRgba8, Format::Undefined, 1, PrimitiveClass::Triangle, false, true));
        kit.list.drawMeshTasksIndirect(args, 1);
    });
}
