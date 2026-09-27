#include "Tint.hlsli"

Texture2D<float4> Scene : register(t0);
SamplerState LinearSampler : register(s0);

float4 MainPS(float2 uv : TEXCOORD0) : SV_Target
{
    const float3 color = Scene.Sample(LinearSampler, uv).rgb;
    return float4(ApplyTint(color), 1.0);
}
