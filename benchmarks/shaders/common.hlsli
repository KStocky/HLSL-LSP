struct MaterialInput {
    float3 normal;
    float2 uv;
};

float3 evaluate_lighting(float3 normal, float3 light_direction, float3 colour) {
    return colour * saturate(dot(normal, light_direction));
}
