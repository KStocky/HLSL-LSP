template<typename T>
T select_value(T left, T right, bool select_right)
{
    return select_right ? right : left;
}

float4 main(float4 position : SV_Position) : SV_Target
{
    Texture2D<float4> texture_value = ResourceDescriptorHeap[0];
    return select_value(position, texture_value.Load(int3(0, 0, 0)), true);
}
