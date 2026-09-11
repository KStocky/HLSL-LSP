using System;
using System.Threading;
using System.Threading.Tasks;
using HlslLsp.VisualStudio.Bootstrap;
using Microsoft.VisualStudio.Text;
using Newtonsoft.Json;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class EffectiveShaderContextTests
{
    [Fact]
    public void Display_UsesSharedDefaultTerminology()
    {
        var context = new EffectiveShaderContextModel
        {
            File = "shader.hlsl",
        };

        Assert.Equal(
            "File: shader.hlsl · Variant: Default · Entry point: Not configured · " +
            "Target profile: Not configured",
            EffectiveShaderContextDisplay.Summary(context));
    }

    [Fact]
    public void Display_ReportsAvailableConfigurationOrigins()
    {
        var context = new EffectiveShaderContextModel
        {
            Origins = new EffectiveContextOriginsModel
            {
                Variant = new EffectiveContextOriginModel { Label = "variant Prod" },
                EntryPoint = new EffectiveContextOriginModel { Label = "editor settings" },
                TargetProfile = new EffectiveContextOriginModel
                {
                    Label = @"C:\shaders\shadertoolsconfig.json",
                },
            },
        };

        Assert.Equal(
            @"Variant — variant Prod; Entry point — editor settings; " +
            @"Target profile — C:\shaders\shadertoolsconfig.json",
            EffectiveShaderContextDisplay.OriginSummary(context));
    }

    [Fact]
    public void IndicatorDisplay_IsCompactAndProvidesFullTooltipContext()
    {
        var context = new EffectiveShaderContextModel
        {
            File = "shader.hlsl",
            ActiveVariant = "Prod",
            EntryPoint = "CSMain",
            TargetProfile = "cs_6_6",
            Origins = new EffectiveContextOriginsModel
            {
                Variant = new EffectiveContextOriginModel { Label = "variant Prod" },
            },
        };

        Assert.Equal(
            "HLSL · Prod · CSMain · cs_6_6",
            HlslEffectiveContextIndicatorDisplay.Compact(context));
        var tooltip = HlslEffectiveContextIndicatorDisplay.Tooltip(context);
        Assert.Contains("File: shader.hlsl", tooltip);
        Assert.Contains("Configuration origins: Variant — variant Prod", tooltip);
        Assert.Contains("Click to select a shader variant.", tooltip);
    }

    [Fact]
    public void Model_DeserializesServerProtocolShape()
    {
        var context = JsonConvert.DeserializeObject<EffectiveShaderContextModel>(
            @"{
                ""documentUri"": ""file:///C:/shaders/main.hlsl"",
                ""file"": ""main.hlsl"",
                ""activeVariant"": ""Prod"",
                ""entryPoint"": ""PSMain"",
                ""targetProfile"": ""ps_6_6"",
                ""origins"": {
                    ""entryPoint"": {
                        ""label"": ""variant Prod"",
                        ""setting"": ""entryPoint"",
                        ""uri"": ""file:///C:/shaders/shadertoolsconfig.json""
                    }
                }
            }");

        Assert.Equal("main.hlsl", context.File);
        Assert.Equal("Prod", context.ActiveVariant);
        Assert.Equal("PSMain", context.EntryPoint);
        Assert.Equal("ps_6_6", context.TargetProfile);
        Assert.Equal("variant Prod", context.Origins.EntryPoint.Label);
    }

    [Fact]
    public async Task Bridge_ForwardsDocumentAndCancellation()
    {
        Uri observedUri = null;
        CancellationToken observedToken = default;
        var expected = new EffectiveShaderContextModel { File = "main.hlsl" };
        EffectiveShaderContextBridge.Register(
            (uri, token) =>
            {
                observedUri = uri;
                observedToken = token;
                return Task.FromResult(expected);
            });
        using var cancellation = new CancellationTokenSource();
        var uri = new Uri("file:///C:/shaders/main.hlsl");

        var actual = await EffectiveShaderContextBridge.RequestAsync(
            uri,
            cancellation.Token);

        Assert.Same(expected, actual);
        Assert.Equal(uri, observedUri);
        Assert.Equal(cancellation.Token, observedToken);
    }

    [Fact]
    public void IndicatorDocumentState_TracksRenameAndSaveAsActionsUntilDisposed()
    {
        var state = new HlslEffectiveContextDocumentState(
            @"C:\shaders\original.hlsl");
        var original = state.CurrentUri;

        Assert.False(state.Apply(new TextDocumentFileActionEventArgs(
            @"C:\shaders\original.hlsl",
            DateTime.UtcNow,
            FileActionTypes.ContentSavedToDisk)));
        Assert.Equal(original, state.CurrentUri);

        Assert.True(state.Apply(new TextDocumentFileActionEventArgs(
            @"C:\shaders\original.hlsl",
            @"C:\shaders\renamed.hlsl",
            DateTime.UtcNow,
            FileActionTypes.DocumentRenamed)));
        Assert.Equal(
            new Uri("file:///C:/shaders/renamed.hlsl"),
            state.CurrentUri);

        Assert.True(state.Apply(new TextDocumentFileActionEventArgs(
            @"C:\shaders\renamed.hlsl",
            @"C:\shaders\saved-as.hlsl",
            DateTime.UtcNow,
            FileActionTypes.DocumentRenamed |
            FileActionTypes.ContentSavedToDisk)));
        Assert.Equal(
            new Uri("file:///C:/shaders/saved-as.hlsl"),
            state.CurrentUri);

        Assert.True(state.Apply(new TextDocumentFileActionEventArgs(
            @"C:\shaders\saved-as.hlsl",
            "\0",
            DateTime.UtcNow,
            FileActionTypes.DocumentRenamed)));
        Assert.Null(state.CurrentUri);

        state.Dispose();
        Assert.Null(state.CurrentUri);
        Assert.False(state.Apply(new TextDocumentFileActionEventArgs(
            @"C:\shaders\saved-as.hlsl",
            @"C:\shaders\after-dispose.hlsl",
            DateTime.UtcNow,
            FileActionTypes.DocumentRenamed)));
        Assert.Null(state.CurrentUri);
    }
}
