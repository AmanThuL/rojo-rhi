#pragma once

#include <catch2/catch_test_macros.hpp>

#include "RHI/CommandList.h"
#include "RHI/Indirect.h"
#include "RHI/ShaderLibrary.h"
#include "RHI/Validate.h"

#include <limits>

#include <limits>

using namespace lmx::rhi;

namespace {
// ShaderLibrary is an interface; pipeline validation only inspects the pointer,
// so a trivial concrete subclass is enough to exercise the happy path.
struct DummyShaderLibrary : ShaderLibrary {};

// Stands in for the CAMetalLayer* the windowing layer supplies; validation only
// checks it against nullptr.
[[maybe_unused]] int dummyNativeLayer = 0;

// Texture is an interface and the helpers under test read nothing but its reported shape, so a
// texture of a given extent, format, and subresource count is expressible without a device.
// readback() is never reached -- no helper calls it -- so it is left empty rather than faked.
struct FakeTexture final : Texture {

    //==================================================================================================================
    FakeTexture(uint32_t width, uint32_t height, Format format = Format::BGRA8Unorm,
                uint32_t mipLevels = 1, uint32_t arrayLayers = 1)
        : m_width(width), m_height(height), m_format(format), m_mipLevels(mipLevels),
          m_arrayLayers(arrayLayers) {}

    //==================================================================================================================
    uint32_t width() const override { return m_width; }

    //==================================================================================================================
    uint32_t height() const override { return m_height; }

    //==================================================================================================================
    Format format() const override { return m_format; }

    //==================================================================================================================
    uint32_t mipLevels() const override { return m_mipLevels; }

    //==================================================================================================================
    uint32_t arrayLayers() const override { return m_arrayLayers; }

    //==================================================================================================================
    void readback(void*, uint64_t) override {}

private:
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    Format m_format = Format::BGRA8Unorm;
    uint32_t m_mipLevels = 1;
    uint32_t m_arrayLayers = 1;
};

// Heap is an interface and validatePlacement reads nothing but its size, so the bound a placement
// is checked against is expressible without a device.
struct FakeHeap final : Heap {

    //==================================================================================================================
    explicit FakeHeap(uint64_t size) : m_size(size) {}

    //==================================================================================================================
    uint64_t size() const override { return m_size; }

private:
    uint64_t m_size = 0;
};

// The same for buffers: the copy, fill, and barrier helpers read nothing but the allocation size,
// and identity comparisons between two of these stand in for "the same buffer twice".
struct FakeBuffer final : Buffer {

    //==================================================================================================================
    explicit FakeBuffer(uint64_t size) : m_size(size) {}

    //==================================================================================================================
    uint64_t size() const override { return m_size; }

    //==================================================================================================================
    void readback(void*, uint64_t) override {}

    //==================================================================================================================
    void write(uint64_t, const void*, uint64_t) override {}

private:
    uint64_t m_size = 0;
};

//======================================================================================================================
// A region covering the whole of one mip level of a square texture, which is what most of the copy
// cases below start from before perturbing one field.
inline TextureCopyRegion wholeLevel(uint32_t extent, uint32_t mipLevel = 0,
                                    uint32_t arrayLayer = 0) {
    const uint32_t levelExtent = mipExtent(extent, mipLevel);
    return {.mipLevel = mipLevel,
            .arrayLayer = arrayLayer,
            .width = levelExtent,
            .height = levelExtent};
}
} // namespace
