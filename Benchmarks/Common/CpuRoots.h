//----------------------------------------------------------------------------------------------------------------------
/// @file CpuRoots.h
/// @brief Declares the root blocks the CPU operation workloads' shaders read and the values both arms and oracles derive.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <array>
#include <cstdint>

namespace rojoRHI::bench {
    // C1 to C3 and C8 render to a target of this many pixels a side, in RGBA8Unorm; C3 adds a D32Float depth target.
    inline constexpr uint32_t kTargetSize = 256;
    inline constexpr uint32_t kTargetPixels = kTargetSize * kTargetSize;
    inline constexpr uint64_t kTargetBytes = uint64_t{ kTargetPixels } * 4;

    // C1's and C2's root, mirrored by DrawRoot in BenchDraw.slang: draw i places a one-pixel triangle at pixel
    // i mod 65,536, row-major from the top-left, and fills it with color.
    struct DrawRoot
    {
        uint32_t index = 0;
        uint32_t reserved0[3] = {};
        float color[4] = {};
        uint32_t reserved1[8] = {};
    };

    static_assert(sizeof(DrawRoot) == 64, "C1's root block is 64 bytes");

    // C4's root, mirrored by StoreRoot in BenchCompute.slang: the dispatch stores value at address.
    struct StoreRoot
    {
        Addr address = {};
        uint32_t value = 0;
        uint32_t reserved = 0;
    };

    static_assert(sizeof(StoreRoot) == 16, "C4's root block is 16 bytes");

    // C6's root, mirrored by CounterRoot in BenchCompute.slang: the address of the counter each dispatch adds one to.
    struct CounterRoot
    {
        Addr counter = {};
    };

    static_assert(sizeof(CounterRoot) == 8, "C6's root block is 8 bytes");

    // Draw i's color in RGBA8Unorm: the low, middle and high bytes of i and an opaque alpha, so draws i and i + 65,536,
    // which share a pixel, differ, and no draw matches the cleared target.
    inline constexpr std::array<uint8_t, 4> drawColor(uint32_t index)
    {
        return { static_cast<uint8_t>(index & 0xFF), static_cast<uint8_t>((index >> 8) & 0xFF), static_cast<uint8_t>((index >> 16) & 0xFF), 255 };
    }

    // Draw i's root. Each channel is a whole number of 255ths, which RGBA8Unorm stores exactly.
    inline constexpr DrawRoot drawRoot(uint32_t index)
    {
        const std::array<uint8_t, 4> color = drawColor(index);
        DrawRoot root;
        root.index = index;
        for (uint32_t channel = 0; channel < 4; ++channel)
        {
            root.color[channel] = static_cast<float>(color[channel]) / 255.0f;
        }
        return root;
    }

    // The value C4's dispatch i stores.
    inline constexpr uint32_t storeValue(uint32_t index)
    {
        return index * 2'654'435'761u + 1u;
    }

    // An Addr from a native GPU address.
    inline constexpr Addr addrOf(uint64_t address)
    {
        return Addr{ static_cast<uint32_t>(address & 0xFFFF'FFFFu), static_cast<uint32_t>(address >> 32) };
    }

    // BenchPlain.slang's color in RGBA8Unorm and its triangle's depth.
    inline constexpr std::array<uint8_t, 4> kPlainColor{ 51, 102, 153, 255 };
    inline constexpr float kPlainDepth = 0.5f;

    // True when the centre of pixel (x, y) lies inside BenchPlain.slang's triangle, whose corners are (64, 192),
    // (192, 192) and (128, 64) in pixels; no centre lies on an edge.
    inline constexpr bool insidePlainTriangle(uint32_t x, uint32_t y)
    {
        const double px = x + 0.5;
        const double py = y + 0.5;
        const auto side = [&](double ax, double ay, double bx, double by) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); };
        const double a = side(64.0, 192.0, 192.0, 192.0);
        const double b = side(192.0, 192.0, 128.0, 64.0);
        const double c = side(128.0, 64.0, 64.0, 192.0);
        return (a < 0.0 && b < 0.0 && c < 0.0) || (a > 0.0 && b > 0.0 && c > 0.0);
    }
} // namespace rojoRHI::bench
