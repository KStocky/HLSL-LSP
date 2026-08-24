[[vk::binding(0, 0)]] RWStructuredBuffer<uint> Output;

[numthreads(8, 8, 1)]
void MainCS(uint3 dispatch_id : SV_DispatchThreadID)
{
    Output[dispatch_id.x] = dispatch_id.x;
}
