//----------------------------------------------------------------------------------------------------------------------
/// @file ImGui.slang
/// @brief The Dear ImGui renderer's entries: a vertex entry that pulls one `ImDrawVert` from the address the root names
/// and maps it to clip space, and a fragment entry that modulates the vertex colour by one sampled view.
//----------------------------------------------------------------------------------------------------------------------

import RojoRHI;

// The layout of `ImDrawVert`: 20 bytes, every member at a multiple of 4.
struct ImGuiVertex
{
    float2 pos;
    float2 uv;
    uint color;
};

// The layout of `rojoRHI::imgui::detail::Root`: 32 bytes.
struct ImGuiRoot
{
    rojoRHI::Addr vertices;
    float2 scale;
    float2 translate;
    uint texture;
    uint sampler;
};

struct ImGuiVaryings
{
    float4 position : SV_Position;
    float4 color : COLOR0;
    float2 uv : TEXCOORD0;
};

[shader("vertex")]
ImGuiVaryings imguiVertex(rojoRHI::VertexIds ids)
{
    ImGuiRoot root = rojoRHI::root<ImGuiRoot>();
    ImGuiVertex vertex = rojoRHI::load<ImGuiVertex>(root.vertices, rojoRHI::vertexIndex(ids));
    uint c = vertex.color;
    ImGuiVaryings output;
    output.position = float4(vertex.pos * root.scale + root.translate, 0.0, 1.0);
    output.color = float4(float((c >> 0) & 0xFF), float((c >> 8) & 0xFF), float((c >> 16) & 0xFF), float((c >> 24) & 0xFF)) / 255.0;
    output.uv = vertex.uv;
    return output;
}

[shader("fragment")]
float4 imguiFragment(ImGuiVaryings input) : SV_Target
{
    ImGuiRoot root = rojoRHI::root<ImGuiRoot>();
    return input.color * rojoRHI::texture2D(root.texture).Sample(rojoRHI::sampler(root.sampler), input.uv);
}
