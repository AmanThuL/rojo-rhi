//----------------------------------------------------------------------------------------------------------------------
/// @file CommandListDeathTests.cpp
/// @brief Death tests for the valid-usage rules of acquiring, ending, submitting and uploading to command lists.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Support/Death.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>

using namespace rojoRHI;

namespace {
    constexpr std::array<std::byte, 16> kBytes = {};

    //==================================================================================================================
    void submit(Device device, CommandList list, QueueType type = QueueType::Graphics)
    {
        const std::array<CommandList, 1> lists{ list };
        device.queue(type).submit(lists);
    }
} // namespace

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0001 a list is acquired with no frame open", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0001", [] {
        Device device = *createDevice();
        (void)device.acquireCommandList(QueueType::Graphics, "test.list");
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0001 a list is acquired after the frame ended", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0001", [] {
        Device device = *createDevice();
        device.beginFrame();
        device.endFrame();
        (void)device.acquireCommandList(QueueType::Graphics, "test.list");
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-cmd-0002 a list ends twice", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-cmd-0002", [] {
        Device device = *createDevice();
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        list.end();
        list.end();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-core-0004 a default-constructed CommandList ends", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-core-0004", [] {
        CommandList list;
        list.end();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-core-0004 a default-constructed CommandList uploads frame data", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-core-0004", [] {
        CommandList list;
        (void)list.uploadFrameData(kBytes);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-core-0004 a default-constructed CommandList is submitted", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-core-0004", [] {
        Device device = *createDevice();
        device.beginFrame();
        submit(device, CommandList{});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-core-0005 a list of another device is submitted", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-core-0005", [] {
        Device first = *createDevice();
        Device second = *createDevice();
        first.beginFrame();
        second.beginFrame();
        CommandList list = first.acquireCommandList(QueueType::Graphics, "test.list");
        list.end();
        submit(second, list);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-queue-0001 a list that has not ended is submitted", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-queue-0001", [] {
        Device device = *createDevice();
        device.beginFrame();
        submit(device, device.acquireCommandList(QueueType::Graphics, "test.list"));
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-queue-0001 a list is submitted twice", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-queue-0001", [] {
        Device device = *createDevice();
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        list.end();
        submit(device, list);
        submit(device, list);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-queue-0001 a list is submitted in a later frame", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-queue-0001", [] {
        Device device = *createDevice();
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        list.end();
        submit(device, list);
        device.endFrame();
        device.beginFrame();
        submit(device, list);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-queue-0001 a list is submitted with no frame open", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-queue-0001", [] {
        Device device = *createDevice();
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        list.end();
        submit(device, list);
        device.endFrame();
        submit(device, list);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-queue-0002 a Graphics list is submitted to the Compute queue", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-queue-0002", [] {
        Device device = *createDevice();
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        list.end();
        submit(device, list, QueueType::Compute);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-device-0005 a frame ends with a list not submitted", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-device-0005", [] {
        Device device = *createDevice();
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        list.end();
        device.endFrame();
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0015 frame data of no bytes is uploaded", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0015", [] {
        Device device = *createDevice();
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        (void)list.uploadFrameData({});
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0016 frame data is uploaded at a non-power-of-two alignment", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0016", [] {
        Device device = *createDevice();
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        (void)list.uploadFrameData(kBytes, 24);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0016 frame data is uploaded at an alignment of zero", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0016", [] {
        Device device = *createDevice();
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        (void)list.uploadFrameData(kBytes, 0);
    });
}

//======================================================================================================================
TEST_CASE("VU-rojo-buffer-0017 frame data is uploaded after the frame ended", "[contract][death]")
{
    ROJORHI_EXPECT_DEATH("VU-rojo-buffer-0017", [] {
        Device device = *createDevice();
        device.beginFrame();
        CommandList list = device.acquireCommandList(QueueType::Graphics, "test.list");
        list.end();
        submit(device, list);
        device.endFrame();
        (void)list.uploadFrameData(kBytes);
    });
}
