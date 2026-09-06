using System.Linq;
using HlslLsp.VisualStudio.Bootstrap;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class MemoryLayoutDisplayNameTests
{
    [Theory]
    [InlineData("", "values", null, false, "values")]
    [InlineData("values", "[1]", "array", false, "values[1]")]
    [InlineData("items[1]", "colour", "record", false, "items[1].colour")]
    [InlineData("transform", "[0]", "matrix", true, "transform.row[0]")]
    [InlineData("transform", "[1]", "matrix", false, "transform.column[1]")]
    public void Qualify_PreservesOwningHierarchy(
        string parentName,
        string childName,
        string parentKind,
        bool parentRowMajor,
        string expected)
    {
        Assert.Equal(
            expected,
            MemoryLayoutDisplayName.Qualify(
                parentName,
                childName,
                parentKind,
                parentRowMajor));
    }

    [Fact]
    public void ByteScale_LabelsAbsoluteFourByteBoundaries()
        => Assert.Equal(
            new long[] { 32, 36, 40, 44, 48 },
            MemoryLayoutByteScale.Labels(32).ToArray());
}
