using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class HlslClassifierTests
{
    [Theory]
    [InlineData("float2x2")]
    [InlineData("float3x4")]
    [InlineData("float4x1")]
    [InlineData("half2x3")]
    [InlineData("double4x4")]
    [InlineData("int1x2")]
    [InlineData("min12int2x3")]
    [InlineData("uint16_t3x4")]
    public void IsBuiltInType_AcceptsMatrixAliases(string identifier)
        => Assert.True(HlslClassifier.IsBuiltInType(identifier));

    [Theory]
    [InlineData("float0x4")]
    [InlineData("float3x5")]
    [InlineData("float3x")]
    [InlineData("float3x4Value")]
    public void IsBuiltInType_RejectsInvalidMatrixAliases(string identifier)
        => Assert.False(HlslClassifier.IsBuiltInType(identifier));
}
