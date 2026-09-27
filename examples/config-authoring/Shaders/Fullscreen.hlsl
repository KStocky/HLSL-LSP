struct VertexToPixel
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

VertexToPixel MainVS(uint vertexId : SV_VertexID)
{
    const float2 positions[3] = {
        float2(-1.0, -1.0),
        float2(-1.0,  3.0),
        float2( 3.0, -1.0)
    };

    VertexToPixel output;
    output.position = float4(positions[vertexId], 0.0, 1.0);
    output.uv = positions[vertexId] * 0.5 + 0.5;
    return output;
}
