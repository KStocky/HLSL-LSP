#include "common.hlsli"

Texture2D<float4> base_colour : register(t0);
SamplerState linear_sampler : register(s0);

struct PixelInput {
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

template<typename T>
T weighted_sum(T left, T right, float weight) {
    return lerp(left, right, weight);
}

float4 main(PixelInput input) : SV_Target {
    MaterialInput material = {normalize(input.normal), input.uv};
    float4 sampled = base_colour.Sample(linear_sampler, material.uv);
    float3 lit = evaluate_lighting(material.normal, normalize(float3(1.0, 2.0, 3.0)),
                                   sampled.rgb);
    return float4(weighted_sum(lit, sampled.rgb, 0.25), sampled.a);
}
