#include <catch2/catch_test_macros.hpp>

#include "RHI/CommandList.h"
#include "RHI/GpuAddress.h"

#include <cstdint>
#include <type_traits>
#include <vector>

using namespace lmx::rhi;

namespace {

template <typename T>
concept SupportsTypedFrameData =
    requires(CommandList& commands, const T& value) { commands.bindFrameData(0, value); };

// What the public overloads forward is the whole of their contract, and none of it needs a device:
// the convenience overload supplies the default alignment and the typed one derives size and
// alignment from T. Only bindFrameData is implemented; everything else on the interface is a
// no-op, because nothing here records a pass.
struct ForwardingCommandList final : CommandList {
    struct Request {
        uint32_t slot = 0;
        const void* data = nullptr;
        uint64_t size = 0;
        uint64_t alignment = 0;
    };
    std::vector<Request> requests;
    uint64_t nextAddress = 4096;

    //==================================================================================================================
    GpuAddress bindFrameData(uint32_t slot, const void* data, uint64_t size,
                             uint64_t alignment) override {
        requests.push_back({slot, data, size, alignment});
        nextAddress += alignment;
        return GpuAddress{nextAddress};
    }
    using CommandList::bindFrameData;

    //==================================================================================================================
    void temporalScale(TemporalScaler&, const TemporalScaleParams&) override {}

    //==================================================================================================================
    void beginRenderPass(const RenderPassDesc&) override {}

    //==================================================================================================================
    void endRenderPass() override {}

    //==================================================================================================================
    void beginComputePass(std::string_view) override {}

    //==================================================================================================================
    void endComputePass() override {}

    //==================================================================================================================
    void bindComputePipeline(ComputePipeline&) override {}

    //==================================================================================================================
    void bindStorageBuffer(uint32_t, Buffer&, StorageAccess) override {}

    //==================================================================================================================
    void bindStorageTexture(uint32_t, Texture&, const TextureViewDesc&, StorageAccess) override {}

    //==================================================================================================================
    void dispatch(uint32_t, uint32_t, uint32_t) override {}

    //==================================================================================================================
    void dispatchIndirect(Buffer&, uint64_t) override {}

    //==================================================================================================================
    void beginCopyPass(std::string_view) override {}

    //==================================================================================================================
    void endCopyPass() override {}

    //==================================================================================================================
    void copyBuffer(Buffer&, uint64_t, Buffer&, uint64_t, uint64_t) override {}

    //==================================================================================================================
    void copyBufferToTexture(Buffer&, const BufferTextureLayout&, Texture&,
                             const TextureCopyRegion&) override {}

    //==================================================================================================================
    void copyTextureToBuffer(Texture&, const TextureCopyRegion&, Buffer&,
                             const BufferTextureLayout&) override {}

    //==================================================================================================================
    void copyTexture(Texture&, const TextureCopyRegion&, Texture&,
                     const TextureCopyRegion&) override {}

    //==================================================================================================================
    void fillBuffer(Buffer&, uint64_t, uint64_t, uint8_t) override {}

    //==================================================================================================================
    void bindPipeline(GraphicsPipeline&) override {}

    //==================================================================================================================
    void bindBuffer(uint32_t, Buffer&) override {}

    //==================================================================================================================
    void bindTexture(uint32_t, Texture&, const TextureViewDesc&) override {}

    //==================================================================================================================
    void bindSampler(uint32_t, Sampler&) override {}

    //==================================================================================================================
    void draw(uint32_t, uint32_t) override {}

    //==================================================================================================================
    void drawIndexed(Buffer&, uint32_t, uint32_t) override {}

    //==================================================================================================================
    void drawIndirect(Buffer&, uint64_t) override {}

    //==================================================================================================================
    void drawIndexedIndirect(Buffer&, Buffer&, uint64_t) override {}

    //==================================================================================================================
    void textureBarrier(Texture&, const TextureSubresourceRange&, TextureUse, TextureUse,
                        BarrierOptions) override {}

    //==================================================================================================================
    void bufferBarrier(Buffer&, const BufferRange&, BufferUse, BufferUse, BarrierOptions) override {
    }
};

struct PlainBlock {
    uint32_t a = 0;
    uint32_t b = 0;
};

// Over-aligned past kFrameDataAlignment, which is the only case in which alignof(T) rather than the
// default decides where the block lands.
struct alignas(512) OverAlignedBlock {
    float values[4] = {};
};

// A block naming another block, which is the composition the returned address exists for.
struct AddressCarryingBlock {
    GpuAddress child;
    uint32_t count = 0;
};

} // namespace

//======================================================================================================================
// Zero is the null address and is what a default-constructed value holds, so a caller can tell an
// address it never obtained from one it did without a sentinel of its own.
TEST_CASE("a default GpuAddress is the null address", "[rhi]") {
    constexpr GpuAddress unset;
    REQUIRE(unset.value == 0);
    REQUIRE_FALSE(unset.isValid());

    constexpr GpuAddress named{0x1000};
    REQUIRE(named.isValid());
    REQUIRE(named == GpuAddress{0x1000});
    REQUIRE_FALSE(named == unset);
}

//======================================================================================================================
// The properties that let an address be copied into a block the GPU reads. They are static_asserts
// in the header too; restating them here is what fails the suite rather than one translation unit
// if the value ever grows a member that breaks them.
TEST_CASE("GpuAddress stays a plain value", "[rhi]") {
    STATIC_REQUIRE(std::is_standard_layout_v<GpuAddress>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<GpuAddress>);
    STATIC_REQUIRE(sizeof(GpuAddress) == sizeof(uint64_t));
}

//======================================================================================================================
// A pointer is itself trivially copyable, so the generic constraint alone would accept a CPU
// address as if it were a shader-readable value block. The dedicated deleted overload closes that
// hole while the raw data/size operation remains available for uploading the pointee's bytes.
TEST_CASE("the typed frame-data overload rejects CPU pointers", "[rhi]") {
    STATIC_REQUIRE_FALSE(SupportsTypedFrameData<uint32_t*>);
    STATIC_REQUIRE(SupportsTypedFrameData<uint32_t>);
}

//======================================================================================================================
// The default alignment comes from the convenience overload rather than from a defaulted parameter
// on the virtual, so this is the case that fails if it ever moves.
TEST_CASE("the untyped frame-data overload supplies the default alignment", "[rhi]") {
    ForwardingCommandList commands;
    const uint32_t block[4] = {1, 2, 3, 4};

    commands.bindFrameData(3, block, sizeof(block));

    REQUIRE(commands.requests.size() == 1);
    REQUIRE(commands.requests[0].slot == 3);
    REQUIRE(commands.requests[0].data == block);
    REQUIRE(commands.requests[0].size == sizeof(block));
    REQUIRE(commands.requests[0].alignment == kFrameDataAlignment);
}

//======================================================================================================================
// The typed overload copies exactly sizeof(T) and asks for max(kFrameDataAlignment, alignof(T)):
// an ordinary block takes the default, and one that over-aligns itself takes its own.
TEST_CASE("the typed frame-data overload derives size and alignment from T", "[rhi]") {
    ForwardingCommandList commands;
    const PlainBlock plain{7, 9};
    const OverAlignedBlock wide;

    commands.bindFrameData(0, plain);
    commands.bindFrameData(1, wide);

    REQUIRE(commands.requests.size() == 2);
    REQUIRE(commands.requests[0].data == &plain);
    REQUIRE(commands.requests[0].size == sizeof(PlainBlock));
    REQUIRE(commands.requests[0].alignment == kFrameDataAlignment);

    REQUIRE(commands.requests[1].slot == 1);
    REQUIRE(commands.requests[1].size == sizeof(OverAlignedBlock));
    REQUIRE(commands.requests[1].alignment == alignof(OverAlignedBlock));
    REQUIRE(commands.requests[1].alignment > kFrameDataAlignment);
}

//======================================================================================================================
// The composition the returned address exists for: one block names another uploaded earlier in the
// same frame. A block holding a GpuAddress is still trivially copyable, which is what lets the
// typed overload accept it at all.
TEST_CASE("a frame-data block can carry the address of an earlier block", "[rhi]") {
    STATIC_REQUIRE(std::is_trivially_copyable_v<AddressCarryingBlock>);

    ForwardingCommandList commands;
    const uint32_t leaf[2] = {5, 6};

    const GpuAddress child = commands.bindFrameData(0, leaf, sizeof(leaf));
    REQUIRE(child.isValid());

    const AddressCarryingBlock parent{.child = child, .count = 2};
    commands.bindFrameData(1, parent);

    REQUIRE(commands.requests.size() == 2);
    REQUIRE(commands.requests[1].size == sizeof(AddressCarryingBlock));
    REQUIRE(static_cast<const AddressCarryingBlock*>(commands.requests[1].data)->child == child);
}
