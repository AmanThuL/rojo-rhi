//----------------------------------------------------------------------------------------------------------------------
/// @file ImGuiRoot.h
/// @brief Declares the root block the ImGui shaders read and pins its layout and the vertex layout they pull.
//----------------------------------------------------------------------------------------------------------------------
#pragma once

#include <rojoRHI/RHI.h>

#include <imgui.h>

#include <cstddef>
#include <cstdint>

namespace rojoRHI::imgui::detail {
    /// The root block of one ImGui draw command, flat and 32 bytes, read by both entries of `ImGui.slang`.
    struct Root
    {
        Addr vertices;      ///< The command's first vertex: the list's vertex allocation plus `VtxOffset` vertices.
        float scale[2];     ///< ImGui pixels to clip space, per axis.
        float translate[2]; ///< Added after the scale.
        uint32_t texture;   ///< The view index of the command's texture.
        uint32_t sampler;   ///< The sampler index.
    };

    // The shader side declares the same members at the same offsets; a drift shows in the GPU pixel case.
    static_assert(sizeof(Root) == 32);
    static_assert(offsetof(Root, vertices) == 0);
    static_assert(offsetof(Root, scale) == 8);
    static_assert(offsetof(Root, translate) == 16);
    static_assert(offsetof(Root, texture) == 24);
    static_assert(offsetof(Root, sampler) == 28);

    // The vertex entry pulls ImDrawVert as {float2 pos; float2 uv; uint color} at a 20-byte stride.
    static_assert(sizeof(ImDrawVert) == 20);
    static_assert(offsetof(ImDrawVert, pos) == 0);
    static_assert(offsetof(ImDrawVert, uv) == 8);
    static_assert(offsetof(ImDrawVert, col) == 16);
} // namespace rojoRHI::imgui::detail
