using System;

using HlslLsp.VisualStudio.Bootstrap;

using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class HlslCommandContextTests : IDisposable
{
    private readonly Uri documentUri = new("file:///C:/shaders/test.hlsl");
    private readonly object cacheOwner = new();

    public HlslCommandContextTests()
    {
        HlslCommandContextCache.Clear();
    }

    public void Dispose()
    {
        HlslCommandContextCache.Clear();
    }

    [Fact]
    public void Cache_RequiresExactDocumentAndPosition()
    {
        var context = new HlslCommandContextModel
        {
            CallHierarchyAvailable = true,
            CallableName = "helper",
        };
        HlslCommandContextCache.Publish(cacheOwner, documentUri, 4, 7, context);

        Assert.True(
            HlslCommandContextCache.TryGet(
                documentUri,
                4,
                7,
                out var result));
        Assert.Same(context, result);
        Assert.False(
            HlslCommandContextCache.TryGet(
                documentUri,
                4,
                8,
                out _));
        Assert.False(
            HlslCommandContextCache.TryGet(
                new Uri("file:///C:/shaders/other.hlsl"),
                4,
                7,
                out _));
    }

    [Fact]
    public void Cache_KeepsIndependentContextsForSplitViews()
    {
        var otherOwner = new object();
        HlslCommandContextCache.Publish(
            cacheOwner,
            documentUri,
            2,
            3,
            new HlslCommandContextModel { CallableName = "first" });
        HlslCommandContextCache.Publish(
            otherOwner,
            documentUri,
            8,
            9,
            new HlslCommandContextModel { CallableName = "second" });

        HlslCommandContextCache.Remove(cacheOwner);

        Assert.False(
            HlslCommandContextCache.TryGet(
                documentUri,
                2,
                3,
                out _));
        Assert.True(
            HlslCommandContextCache.TryGet(
                documentUri,
                8,
                9,
                out var remaining));
        Assert.Equal("second", remaining.CallableName);
    }

    [Fact]
    public void Invalidate_ClearsCacheAndAdvancesRevision()
    {
        HlslCommandContextCache.Publish(
            cacheOwner,
            documentUri,
            1,
            2,
            new HlslCommandContextModel());
        var revision = HlslCommandContextBridge.Revision;

        HlslCommandContextBridge.Invalidate();

        Assert.True(HlslCommandContextBridge.Revision > revision);
        Assert.False(
            HlslCommandContextCache.TryGet(
                documentUri,
                1,
                2,
                out _));
    }

    [Fact]
    public void Cache_RejectsPublishFromAnInvalidatedRequest()
    {
        var revision = HlslCommandContextCache.Revision;
        HlslCommandContextCache.Clear();

        var published = HlslCommandContextCache.PublishIfCurrent(
            revision,
            cacheOwner,
            documentUri,
            1,
            2,
            new HlslCommandContextModel());

        Assert.False(published);
        Assert.False(
            HlslCommandContextCache.TryGet(
                documentUri,
                1,
                2,
                out _));
    }

    [Fact]
    public void DocumentCommand_IsAvailableWithoutSymbolContext()
    {
        var result = HlslCommandPresentation.Evaluate(
            HlslCommandKind.Compilation,
            hlslEditor: true,
            contextKnown: false,
            context: null);

        Assert.True(result.Visible);
        Assert.True(result.Enabled);
        Assert.Equal("Shader Compilation", result.Text);
    }

    [Fact]
    public void ContextCommand_IsDisabledWhileContextIsUnknown()
    {
        var result = HlslCommandPresentation.Evaluate(
            HlslCommandKind.MemoryLayout,
            hlslEditor: true,
            contextKnown: false,
            context: null);

        Assert.True(result.Visible);
        Assert.False(result.Enabled);
        Assert.Equal("Memory Layout", result.Text);
    }

    [Fact]
    public void ContextCommand_IsHiddenWhenKnownToBeInapplicable()
    {
        var result = HlslCommandPresentation.Evaluate(
            HlslCommandKind.EntryPointDataFlow,
            hlslEditor: true,
            contextKnown: true,
            context: new HlslCommandContextModel());

        Assert.False(result.Visible);
        Assert.False(result.Enabled);
        Assert.Equal("Entry-Point Data Flow", result.Text);
    }

    [Fact]
    public void ApplicableContextCommand_UsesTargetAwareLabel()
    {
        var result = HlslCommandPresentation.Evaluate(
            HlslCommandKind.CallHierarchy,
            hlslEditor: true,
            contextKnown: true,
            context: new HlslCommandContextModel
            {
                CallHierarchyAvailable = true,
                CallableName = "helper",
            });

        Assert.True(result.Visible);
        Assert.True(result.Enabled);
        Assert.Equal("Call Hierarchy for helper", result.Text);
    }


    [Fact]
    public void DocumentCommand_UsesEffectiveConfigurationLabel()
    {
        var result = HlslCommandPresentation.Evaluate(
            HlslCommandKind.OpenEffectiveConfiguration,
            hlslEditor: true,
            contextKnown: false,
            context: null);

        Assert.True(result.Visible);
        Assert.True(result.Enabled);
        Assert.Equal("Open Effective Configuration", result.Text);
    }

    [Fact]
    public void NonHlslEditor_HidesAllCommandsAndRestoresBaseLabel()
    {
        var result = HlslCommandPresentation.Evaluate(
            HlslCommandKind.ComputeVisualization,
            hlslEditor: false,
            contextKnown: true,
            context: new HlslCommandContextModel
            {
                ComputeVisualizationAvailable = true,
                EntryPoint = "main",
            });

        Assert.False(result.Visible);
        Assert.False(result.Enabled);
        Assert.Equal("Compute Visualization", result.Text);
    }
}
