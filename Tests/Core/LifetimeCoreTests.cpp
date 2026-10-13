//----------------------------------------------------------------------------------------------------------------------
/// @file LifetimeCoreTests.cpp
/// @brief Tests that the references of an unsubmitted list are released when its slot is recycled.
//----------------------------------------------------------------------------------------------------------------------
#include <rojoRHI/RHI.h>

#include "Core/CommandLists.h"
#include "Core/DeviceCore.h"
#include "Support/PassKit.h"

#include <catch2/catch_test_macros.hpp>

#include <array>

using namespace rojoRHI;
using test::PassKit;

//======================================================================================================================
TEST_CASE("a list recycled without a submission leaves no references behind", "[core]")
{
#ifdef NDEBUG
    SKIP("validation exists only in a Debug build");
#else
    PassKit kit(QueueType::Graphics, true);
    const Buffer source = kit.buffer(BufferUsage::None);
    const Buffer target = kit.buffer(BufferUsage::None);
    kit.list.copyBuffer(source, 0, target, 0, 16);
    // A frame cannot end with a list unsubmitted, so the slot is recycled directly, as the backend's pool does for a slot a later
    // frame reuses; the list that held the references was never submitted.
    core::DeviceCore& device = core::coreOf(kit.device);
    core::beginList(core::coreOf(kit.list), device, QueueType::Graphics, "test.recycled");
    device.noteListSubmitted();
    kit.device.destroy(source);
    kit.device.destroy(target);
    kit.finish();
#endif
}
