//----------------------------------------------------------------------------------------------------------------------
/// @file Metal4Readback.cpp
/// @brief Implements the test helpers that read any Metal 4 buffer or texture back to the CPU.
//----------------------------------------------------------------------------------------------------------------------

#include "Support/Metal4/Metal4Readback.h"

#include "Core/Buffers.h"
#include "Core/Formats.h"
#include "Core/Records.h"
#include "Core/Textures.h"
#include "Metal4Buffers.h"
#include "Metal4Common.h"
#include "Metal4Device.h"

#include <catch2/catch_test_macros.hpp>

#include <Metal/Metal.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rojoRHI::test {
    //==================================================================================================================
    // Fails the running case through Catch2, so it is called from a test thread.
    std::vector<std::byte> readBuffer(Device device, Buffer buffer)
    {
        DeviceImpl& impl = DeviceImpl::of(device);
        const core::BufferRecord& record = core::checkedBuffer(impl.core, buffer);
        const auto* payload = static_cast<const metal4::BufferPayload*>(record.native);
        REQUIRE(payload != nullptr);
        MTL::Buffer* source = payload->buffer;
        const uint64_t size = record.size;
        const metal4::AutoreleasePool pool;
        MTL::Buffer* target = impl.device->newBuffer(size, MTL::ResourceStorageModeShared);
        REQUIRE(target != nullptr);
        const Result<void> copied = impl.stager.copy(impl, source, target, size);
        std::vector<std::byte> bytes(size);
        std::memcpy(bytes.data(), target->contents(), size);
        target->release();
        REQUIRE(copied.has_value());
        return bytes;
    }

    //==================================================================================================================
    // Fails the running case through Catch2, so it is called from a test thread.
    std::vector<std::byte> readTexture(Device device, Texture texture, uint32_t mip, uint32_t layer)
    {
        DeviceImpl& impl = DeviceImpl::of(device);
        const core::TextureRecord* record = impl.core.pool<Texture>().checked(texture, false);
        REQUIRE(record != nullptr);
        auto* source = static_cast<MTL::Texture*>(record->native);
        REQUIRE(source != nullptr);
        REQUIRE(record->samples == 1);
        const core::FormatLayout layout = core::formatLayout(record->format);
        REQUIRE(layout.blockBytes != 0);
        const Extent3D size = core::mipExtent(record->extent, mip);
        const uint64_t bytesPerRow = (uint64_t{ size.width } + layout.blockWidth - 1) / layout.blockWidth * layout.blockBytes;
        const uint64_t bytesPerImage = bytesPerRow * ((uint64_t{ size.height } + layout.blockHeight - 1) / layout.blockHeight);
        const metal4::AutoreleasePool pool;
        MTL::Buffer* target = impl.device->newBuffer(bytesPerImage * size.depth, MTL::ResourceStorageModeShared);
        REQUIRE(target != nullptr);
        const Result<void> copied =
            impl.stager.copyTexture(impl, source, layer, mip, MTL::Size(size.width, size.height, size.depth), target, bytesPerRow, bytesPerImage);
        std::vector<std::byte> bytes(bytesPerImage * size.depth);
        std::memcpy(bytes.data(), target->contents(), bytes.size());
        target->release();
        REQUIRE(copied.has_value());
        return bytes;
    }
} // namespace rojoRHI::test
