using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using HlslLsp.VisualStudio.Bootstrap;
using Nerdbank.Streams;
using Newtonsoft.Json.Linq;
using StreamJsonRpc;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class RuntimeCaptureTests
{
    [Fact]
    public void ReviewRequiresExplicitSelectionOfEligibleEntries()
    {
        var eligible = new RuntimeCaptureEntryModel
        {
            Id = "capture-1",
            Count = 3,
            Review = new RuntimeCaptureReviewModel
            {
                Eligible = true,
                WarningCodes = new[] { "arguments" },
            },
        };
        var logical = new RuntimeCaptureEntryModel
        {
            Id = "capture-2",
            Count = 7,
            Review = new RuntimeCaptureReviewModel { Eligible = false },
        };
        var review = new RuntimeCaptureSelection(new RuntimeCaptureSnapshotModel
        {
            Entries = new[] { eligible, logical },
        });
        Assert.Empty(review.Selected);
        Assert.True(review.CanSelect(eligible));
        Assert.False(review.CanSelect(logical));
        Assert.Throws<ArgumentException>(() => review.SetSelected(logical, true));
        review.SetSelected(eligible, true);
        Assert.Same(eligible, Assert.Single(review.Selected));
        review.SetSelected(eligible, false);
        Assert.Empty(review.Selected);
    }

    [Fact]
    public async Task RpcUsesVersionedRequestsAndExplicitSessionToken()
    {
        var streams = FullDuplexStream.CreatePair();
        using var serverStream = streams.Item1;
        using var clientStream = streams.Item2;
        var target = new CaptureTarget();
        using var serverRpc = new JsonRpc(serverStream, serverStream, target);
        using var clientRpc = new JsonRpc(clientStream);
        serverRpc.StartListening();
        clientRpc.StartListening();
        var client = new HlslLanguageClient(
            "2021", string.Empty, string.Empty,
            (_, _) => Task.CompletedTask, _ => Task.CompletedTask);
        await client.AttachForCustomMessageAsync(clientRpc);

        var started = await client.StartRuntimeCaptureAsync(CancellationToken.None);
        Assert.Equal("private-token", started.Token);
        Assert.Equal(1, target.StartRequest.Value<int>("protocolVersion"));
        var snapshot = await client.SnapshotRuntimeCaptureAsync(
            new Uri("file:///workspace"), CancellationToken.None);
        Assert.Equal("file:///workspace",
            target.SnapshotRequest["workspaceFolder"]?["uri"]?.Value<string>());
        Assert.Equal("Shaders/main.hlsl",
            Assert.Single(snapshot.Entries).Review.Selection.RelativePath);
        var preview = await client.PreviewRuntimeCaptureAsync(
            new Uri("file:///workspace"),
            started.SessionId,
            new[] { "capture-1" },
            "{\"root\":true}",
            9,
            "sha256:original",
            new[] { new RuntimeCaptureVariantModel
                {
                    EntryId = "capture-1",
                    Name = "Quality",
                },
            },
            new[] { new RuntimeCapturePipelineModel
                {
                    Name = "Forward",
                    Source = "user",
                    Stages = new Dictionary<string, string>
                    {
                        ["vertex"] = "capture-1",
                        ["pixel"] = "capture-1",
                    },
                },
            },
            CancellationToken.None);
        Assert.Equal("session-1", target.PreviewRequest.Value<string>("sessionId"));
        Assert.Equal("capture-1",
            target.PreviewRequest["selectedEntryIds"]?[0]?.Value<string>());
        Assert.Equal("Quality",
            target.PreviewRequest["variants"]?[0]?["name"]?.Value<string>());
        Assert.Equal("capture-1",
            target.PreviewRequest["pipelines"]?[0]?["stages"]?["vertex"]?.Value<string>());
        Assert.Equal("{\"root\":true}",
            target.PreviewRequest["existingConfiguration"]?["content"]?.Value<string>());
        Assert.True(preview.Preview.Valid);
        await client.StopRuntimeCaptureAsync(started.Token, CancellationToken.None);
        Assert.Equal("private-token", target.StopRequest.Value<string>("token"));
        Assert.Equal(1, target.StopRequest.Value<int>("protocolVersion"));
    }

    private sealed class CaptureTarget
    {
        public JObject StartRequest { get; private set; }

        public JObject SnapshotRequest { get; private set; }

        public JObject StopRequest { get; private set; }

        public JObject PreviewRequest { get; private set; }

        [JsonRpcMethod("hlsl/capture/start", UseSingleObjectParameterDeserialization = true)]
        public object Start(JObject request)
        {
            StartRequest = request;
            return new
            {
                protocolVersion = 1,
                sessionId = "session-1",
                endpoint = "local-endpoint",
                token = "private-token",
            };
        }

        [JsonRpcMethod("hlsl/capture/snapshot", UseSingleObjectParameterDeserialization = true)]
        public object Snapshot(JObject request)
        {
            SnapshotRequest = request;
            return new
            {
                protocolVersion = 1,
                sessionId = "session-1",
                active = true,
                accepted = 1,
                rejected = 0,
                overflow = 0,
                entries = new[]
                {
                    new
                    {
                        id = "capture-1",
                        count = 1,
                        invocation = new
                        {
                            source = "Shaders/main.hlsl",
                            entryPoint = "Main",
                            targetProfile = "ps_6_6",
                        },
                        review = new
                        {
                            eligible = true,
                            selection = new
                            {
                                relativePath = "Shaders/main.hlsl",
                                entryPoint = "Main",
                                targetProfile = "ps_6_6",
                            },
                        },
                    },
                },
            };
        }

        [JsonRpcMethod("hlsl/capture/preview", UseSingleObjectParameterDeserialization = true)]
        public object Preview(JObject request)
        {
            PreviewRequest = request;
            return new
            {
                protocolVersion = 1,
                sessionId = "session-1",
                selectedEntryIds = new[] { "capture-1" },
                configuration = new
                {
                    uri = "file:///workspace/shadertoolsconfig.json",
                    exists = true,
                    expectedContentVersion = 9,
                    expectedContentHash = "sha256:original",
                },
                preview = new
                {
                    content = "{\"root\":true}",
                    contentHash = "sha256:preview",
                    valid = true,
                    changed = true,
                    errors = Array.Empty<object>(),
                },
            };
        }

        [JsonRpcMethod("hlsl/capture/stop", UseSingleObjectParameterDeserialization = true)]
        public object Stop(JObject request)
        {
            StopRequest = request;
            return new { protocolVersion = 1, active = false };
        }
    }
}
