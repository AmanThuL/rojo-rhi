//----------------------------------------------------------------------------------------------------------------------
/// @file NativeFrameResources.h
/// @brief Declares the native arm's composed-frame resources: textures, buffers, ID tables, the sampler and their dumps.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include "Common/ArmResult.h"
#include "Common/FrameLayout.h"
#include "Native/NativeContext.h"

#include <rojoRHI/RHI.h>

#include <array>
#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace rojoRHI::bench {
    /// A GPU address as the shaders read an Addr: low word first.
    constexpr Addr nativeAddr(uint64_t address)
    {
        return Addr{ .lo = static_cast<uint32_t>(address), .hi = static_cast<uint32_t>(address >> 32) };
    }

    // Every texture and buffer FrameLayout.h lists, created Private (Gpu memory) except readback[s], resident in the
    // context's residency set, with the scene uploaded once. The view ID table holds one MTL::ResourceID per texture: a
    // texture's sampled, storage and depth-aspect views are the texture itself on Metal, so the view index of a texture is
    // its FrameTexture value. The sampler ID table holds the frame's one sampler at index 0.
    class NativeFrameResources
    {
      public:
        explicit NativeFrameResources(NativeContext& context);

        // Setup, before NativeContext::prepareFrames.
        std::expected<void, std::string> create();

        MTL::Texture* texture(FrameTexture texture) const;
        uint32_t view(FrameTexture texture) const;
        uint32_t sampler() const;
        uint64_t viewTable() const;    ///< GPU address of the view ID table, bound at argument-table buffer 1.
        uint64_t samplerTable() const; ///< GPU address of the sampler ID table, bound at argument-table buffer 2.
        MTL::Buffer* instances() const;
        MTL::Buffer* vertices() const;
        MTL::Buffer* indices() const;
        MTL::Buffer* records(uint32_t slot) const;
        MTL::Buffer* readback(uint32_t slot) const;

        // Outside every measurement: copies every texture and records[slot] into a Shared buffer through one command
        // buffer, committed as frame `commitIndex` of the context and waited, and returns the outputs in kOutputs order.
        std::expected<std::vector<Output>, std::string> readOutputs(uint32_t slot, uint32_t commitIndex);

      private:
        std::expected<void, std::string> createTextures();
        std::expected<void, std::string> createBuffers();
        std::expected<void, std::string> createTables();

        NativeContext& m_context;
        std::array<MTL::Texture*, kTextureCount> m_textures{};
        MTL::Buffer* m_instances = nullptr;
        MTL::Buffer* m_vertices = nullptr;
        MTL::Buffer* m_indices = nullptr;
        std::array<MTL::Buffer*, kFrameSlots> m_records{};
        std::array<MTL::Buffer*, kFrameSlots> m_readback{};
        MTL::Buffer* m_viewIds = nullptr;
        MTL::Buffer* m_samplerIds = nullptr;
        MTL::Buffer* m_dump = nullptr; ///< Shared; every texture's padded rows, then records[s].
    };
} // namespace rojoRHI::bench
