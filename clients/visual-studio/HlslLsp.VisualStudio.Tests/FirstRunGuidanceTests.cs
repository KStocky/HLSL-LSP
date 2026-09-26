using System;
using System.IO;
using HlslLsp.VisualStudio.Bootstrap;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class FirstRunGuidanceTests
{
    [Fact]
    public void ConflictPolicyRecognizesOnlyHlslToolsLanguageService()
    {
        Assert.True(
            FirstRunGuidancePolicy.HasHlslToolsConflict(
                new[]
                {
                    "Microsoft.VisualStudio.Shell.15.0",
                    FirstRunGuidancePolicy.HlslToolsAssembly,
                }));
        Assert.False(
            FirstRunGuidancePolicy.HasHlslToolsConflict(
                new[]
                {
                    "Microsoft.VisualStudio.Shell.15.0",
                    "HlslLsp.VisualStudio.Bootstrap",
                }));
    }

    [Fact]
    public void MarkerCanBeClaimedOnlyOnce()
    {
        var directory = Path.Combine(
            Path.GetTempPath(),
            "hlsl-lsp-first-run-tests",
            Guid.NewGuid().ToString("N"));
        try
        {
            var markers = new FirstRunMarkerStore(directory);
            Assert.True(markers.TryClaim("guidance", out var firstError));
            Assert.Null(firstError);
            Assert.False(markers.TryClaim("guidance", out var repeatedError));
            Assert.Null(repeatedError);
            Assert.True(markers.TryClaim("conflict", out var conflictError));
            Assert.Null(conflictError);
        }
        finally
        {
            if (Directory.Exists(directory))
            {
                Directory.Delete(directory, true);
            }
        }
    }
}
