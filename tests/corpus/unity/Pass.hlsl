cbuffer UnityPerMaterial
{
    float4 BaseColor;
};

float4 Fragment(float4 position : SV_Position) : SV_Target
{
#if UNITY_REVERSED_Z
    position.z = 1.0 - position.z;
#endif
    return BaseColor * position.wwww;
}
