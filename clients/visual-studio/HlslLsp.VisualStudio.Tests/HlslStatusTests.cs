using HlslLsp.VisualStudio.Bootstrap;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class HlslStatusTests
{
    [Fact]
    public void Diagnostics_AreConciseAndRedactAbsolutePaths()
    {
        var diagnostics = HlslStatusDisplay.Diagnostics(new HlslStatusSnapshot
        {
            Lifecycle = HlslLifecycleState.Connected,
            Runtime = new HlslDxcRuntimeModel
            {
                Source = "configured",
                Version = "1.9.2607.13",
            },
            Context = new EffectiveShaderContextModel
            {
                File = @"C:\work\shaders\main.hlsl",
                ActiveVariant = "Shipping",
                EntryPoint = "PSMain",
                TargetProfile = "ps_6_7",
                Origins = new EffectiveContextOriginsModel
                {
                    TargetProfile = new EffectiveContextOriginModel
                    {
                        Label = @"C:\work\shadertoolsconfig.json",
                    },
                },
            },
            LastFailure =
                @"The configured runtime C:\private\sdk\dxc was not found.",
        });

        Assert.Contains("Lifecycle: Connected", diagnostics);
        Assert.Contains("Runtime: DXC 1.9.2607.13 (configured)", diagnostics);
        Assert.Contains("Shader: main.hlsl", diagnostics);
        Assert.Contains("Target profile — shadertoolsconfig.json", diagnostics);
        Assert.DoesNotContain(@"C:\work", diagnostics);
        Assert.DoesNotContain(@"C:\private", diagnostics);
        Assert.DoesNotContain("source text", diagnostics);
    }

    [Fact]
    public void RuntimeDisplay_IsCompactForHealthyState()
    {
        Assert.Equal(
            "DXC 1.9.2607.13 (bundled)",
            HlslStatusDisplay.Runtime(new HlslDxcRuntimeModel
            {
                Source = "bundled",
                Version = "1.9.2607.13",
            }));
    }

    [Fact]
    public void SuccessfulConnection_ClearsThePreviousFailure()
    {
        HlslStatusBridge.ReportFailure("Temporary startup failure.");
        HlslStatusBridge.ReportLifecycle(HlslLifecycleState.Connected);

        var snapshot = HlslStatusBridge.Snapshot;
        Assert.Equal(HlslLifecycleState.Connected, snapshot.Lifecycle);
        Assert.Null(snapshot.LastFailure);
    }

    [Fact]
    public void AnalysisFailure_DoesNotReplaceConnectedLifecycle()
    {
        HlslStatusBridge.ReportLifecycle(HlslLifecycleState.Connected);
        HlslStatusBridge.ReportFailure("Could not retrieve shader information.");

        var snapshot = HlslStatusBridge.Snapshot;
        Assert.Equal(HlslLifecycleState.Connected, snapshot.Lifecycle);
        Assert.Equal("Could not retrieve shader information.", snapshot.LastFailure);
    }
}
