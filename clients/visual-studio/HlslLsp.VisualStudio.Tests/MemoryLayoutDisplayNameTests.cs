using System.Linq;
using HlslLsp.VisualStudio.Bootstrap;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class MemoryLayoutDisplayNameTests
{
    [Fact]
    public void ErrorPreservation_RequiresTheExactDocumentPosition()
    {
        var uri = new System.Uri("file:///C:/shaders/main.hlsl");

        Assert.True(MemoryLayoutTargetIdentity.IsSame(uri, 4, 7, uri, 4, 7));
        Assert.False(MemoryLayoutTargetIdentity.IsSame(uri, 4, 7, uri, 5, 7));
        Assert.False(MemoryLayoutTargetIdentity.IsSame(uri, 4, 7, uri, 4, 8));
        Assert.False(
            MemoryLayoutTargetIdentity.IsSame(
                uri,
                4,
                7,
                new System.Uri("file:///C:/shaders/other.hlsl"),
                4,
                7));
    }

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

    [Fact]
    public void TrackingBuffer_RequiresTheCurrentLiveBufferInstance()
    {
        var liveBuffer = new object();

        Assert.True(MemoryLayoutTrackingBuffer.IsCurrent(liveBuffer, liveBuffer));
        Assert.False(MemoryLayoutTrackingBuffer.IsCurrent(liveBuffer, new object()));
        Assert.False(MemoryLayoutTrackingBuffer.IsCurrent(null, liveBuffer));
    }

    [Fact]
    public void RefreshGate_DefersBackgroundRefreshAndInvalidatesEarlierTarget()
    {
        var gate = new MemoryLayoutRefreshGate();

        Assert.True(gate.TryBeginBackgroundRefresh(out var previousTargetGeneration));
        var explicitGeneration = gate.EnterExplicitRequest();

        Assert.False(gate.IsCurrent(previousTargetGeneration));
        Assert.True(gate.IsCurrent(explicitGeneration));
        Assert.False(gate.TryBeginBackgroundRefresh(out _));
        Assert.True(gate.ExitExplicitRequest());

        Assert.True(gate.TryBeginBackgroundRefresh(out var replayGeneration));
        Assert.True(gate.IsCurrent(replayGeneration));
        Assert.False(gate.IsCurrent(explicitGeneration));
    }

    [Fact]
    public void RefreshGate_ReplaysOnlyAfterAllExplicitRequestsComplete()
    {
        var gate = new MemoryLayoutRefreshGate();

        gate.EnterExplicitRequest();
        gate.EnterExplicitRequest();
        Assert.False(gate.TryBeginBackgroundRefresh(out _));

        Assert.False(gate.ExitExplicitRequest());
        Assert.True(gate.ExitExplicitRequest());
    }
}
