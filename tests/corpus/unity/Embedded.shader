Shader "HLSL-LSP/Fixture"
{
    SubShader
    {
        Pass
        {
            HLSLPROGRAM
            float4 Fragment() : SV_Target { return 1.0.xxxx; }
            ENDHLSL
        }
    }
}
