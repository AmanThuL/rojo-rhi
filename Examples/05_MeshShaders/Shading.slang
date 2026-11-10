import RojoRHI;

struct Varyings
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float3 color : COLOR;
};

[shader("fragment")]
float4 terrainFragment(Varyings input) : SV_Target
{
    float light = 0.3 + 0.7 * saturate(dot(normalize(input.normal), normalize(float3(0.3, 1.0, 0.4))));
    return float4(input.color * light, 1.0);
}
