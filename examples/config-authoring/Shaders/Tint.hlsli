float3 ApplyTint(float3 color)
{
#if WARM_GRADE
    return saturate(color * float3(1.0, 0.85, 0.7) + float3(0.06, 0.02, 0.0));
#else
    return saturate(color);
#endif
}
