using HlslLsp.VisualStudio.Bootstrap;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class PreprocessorExplorerDisplayTests
{
    [Fact]
    public void MissingConfiguredTargetPreservesExpansionAndNavigableOrigin()
    {
        var presentation = PreprocessorIncludePresentation.Create(
            new PreprocessorIncludeModel
            {
                ResolvedUri = null,
                ExpandedPath = "/Configured/missing.hlsli",
                ConfigurationOrigin = "variant Debug",
                ConfigurationOriginUri = "file:///workspace/shadertoolsconfig.json",
            });

        Assert.Equal("-", presentation.TargetLabel);
        Assert.Null(presentation.ResolvedUri);
        Assert.Equal("/Configured/missing.hlsli", presentation.ExpandedPath);
        Assert.Equal("variant Debug", presentation.ConfigurationOrigin);
        Assert.Equal(
            "file:///workspace/shadertoolsconfig.json",
            presentation.ConfigurationOriginUri);
    }

    [Fact]
    public void MissingConfiguredTargetPreservesNonNavigableOrigin()
    {
        var presentation = PreprocessorIncludePresentation.Create(
            new PreprocessorIncludeModel
            {
                ExpandedPath = "missing.hlsli",
                ConfigurationOrigin = "editor settings",
            });

        Assert.Equal("-", presentation.TargetLabel);
        Assert.Equal("missing.hlsli", presentation.ExpandedPath);
        Assert.Equal("editor settings", presentation.ConfigurationOrigin);
        Assert.Null(presentation.ConfigurationOriginUri);
    }
}
