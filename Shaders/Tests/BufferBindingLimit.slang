[[vk::binding(15, 0)]] StructuredBuffer<uint> gInput : register(t15);
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> gOutput : register(u0);
[shader("compute")]
[numthreads(1, 1, 1)]
void computeMain() { gOutput[0] = gInput[0]; }
