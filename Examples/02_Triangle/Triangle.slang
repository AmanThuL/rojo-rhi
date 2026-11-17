import RojoRHI;

struct Vertex
{
    float4 position;
    float4 color;
};

struct TriangleRoot
{
    rojoRHI::Addr vertices;
    float angle;
    float aspect;
};

struct Varyings
{
    float4 position : SV_Position;
    float4 color : COLOR0;
};

// VertexIds carries the built-ins, which differ per backend; vertexIndex reads them alike on each.
[shader("vertex")]
Varyings triangleVertex(rojoRHI::VertexIds ids)
{
    TriangleRoot root = rojoRHI::root<TriangleRoot>();
    // Vertex pulling: the vertex index selects an element behind the root's address, so the pipeline has no
    // vertex input.
    Vertex vertex = rojoRHI::load<Vertex>(root.vertices, rojoRHI::vertexIndex(ids));

    float s = sin(root.angle);
    float c = cos(root.angle);
    float2 p = float2(c * vertex.position.x - s * vertex.position.y, s * vertex.position.x + c * vertex.position.y);

    Varyings output;
    output.position = float4(p.x / root.aspect, p.y, 0.0, 1.0);
    output.color = vertex.color;
    return output;
}

[shader("fragment")]
float4 triangleFragment(Varyings input) : SV_Target
{
    return input.color;
}
