using System;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using HlslLsp.VisualStudio.Bootstrap;
using Nerdbank.Streams;
using Newtonsoft.Json.Linq;
using StreamJsonRpc;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class ConfigurationAuthoringTests
{
    [Fact]
    public void SelectionDefaultsToOnlyUnambiguousResolvedProfilesAndAllowsExplicitChoices()
    {
        var result = new ConfigurationAuthoringResultModel
        {
            Discovery = new ConfigurationDiscoveryModel
            {
                Files = new[]
                {
                    new ConfigurationDiscoveryFileModel
                    {
                        RelativePath = "Shaders/a.hlsl",
                        EntryPoints = new[]
                        {
                            new ConfigurationEntryPointModel
                            {
                                Name = "Main",
                                State = "resolved",
                                TargetProfiles = new[] { "ps_6_6" },
                            },
                            new ConfigurationEntryPointModel
                            {
                                Name = "Shared",
                                State = "ambiguous",
                                Explanation = "Several profiles compile.",
                                TargetProfiles = new[] { "vs_6_6", "ps_6_6" },
                            },
                            new ConfigurationEntryPointModel
                            {
                                Name = "NoProfile",
                                State = "unresolved",
                                TargetProfiles = Array.Empty<string>(),
                            },
                        },
                    },
                },
            },
        };
        var model = new ConfigurationSelectionListModel(result);
        Assert.Equal(3, model.Candidates.Count);
        Assert.Single(model.SelectedSelections);
        Assert.Equal("Main", model.SelectedSelections[0].EntryPoint);
        Assert.Equal("Several profiles compile.", model.Candidates[1].Explanation);

        model.SetSelected(model.Candidates[1], true);
        model.SetSelected(model.Candidates[2], true);
        model.SetSelected(model.Candidates[0], false);
        Assert.Equal(
            new[] { "vs_6_6", "ps_6_6" },
            model.SelectedSelections.Select(selection => selection.TargetProfile));
        Assert.All(model.SelectedSelections, selection => Assert.Equal("Shared", selection.EntryPoint));
        Assert.Throws<ArgumentException>(
            () => model.SetSelected(new ConfigurationCandidateModel(), true));
    }

    [Fact]
    public void PreviewConfirmationRequiresValidChangedContentAndFormatsErrors()
    {
        var result = new ConfigurationAuthoringResultModel
        {
            Preview = new ConfigurationPreviewModel
            {
                Changed = true,
                Valid = false,
                Errors = new[]
                {
                    new ConfigurationValidationErrorModel
                    {
                        Code = "invalid",
                        Field = "hlsl.fileGroups",
                        Message = "Expected an array",
                    },
                },
            },
        };
        Assert.False(ConfigurationPreviewValidation.CanConfirm(result));
        Assert.Equal(
            "hlsl.fileGroups: Expected an array [invalid]",
            ConfigurationPreviewValidation.FormatErrors(result));
        result.Preview.Valid = true;
        Assert.True(ConfigurationPreviewValidation.CanConfirm(result));
        result.Preview.Changed = false;
        Assert.False(ConfigurationPreviewValidation.CanConfirm(result));
        Assert.False(ConfigurationPreviewValidation.CanConfirm(null));
    }

    [Fact]
    public async Task RequestSerializesOptionalFieldsAndDeserializesDiscoveryAndPreview()
    {
        var streams = FullDuplexStream.CreatePair();
        using var serverStream = streams.Item1;
        using var clientStream = streams.Item2;
        var capture = new AuthoringTarget();
        using var serverRpc = new JsonRpc(serverStream, serverStream, capture);
        using var clientRpc = new JsonRpc(clientStream);
        serverRpc.StartListening();
        clientRpc.StartListening();
        var client = new HlslLanguageClient(
            "2021",
            string.Empty,
            string.Empty,
            (_, _) => Task.CompletedTask,
            _ => Task.CompletedTask);
        await client.AttachForCustomMessageAsync(clientRpc);

        var initial = await client.GetConfigurationAuthoringAsync(
            new Uri("file:///workspace"),
            null,
            null,
            null,
            null,
            CancellationToken.None);
        Assert.Equal(1, capture.Request.Value<int>("protocolVersion"));
        Assert.Equal("file:///workspace", capture.Request["workspaceFolder"]?["uri"]?.Value<string>());
        Assert.Null(capture.Request["existingConfiguration"]);
        Assert.Null(capture.Request["selections"]);
        Assert.Equal("Shaders/a.hlsl", initial.Discovery.Files[0].RelativePath);
        Assert.Equal("ambiguous", initial.Discovery.Files[0].EntryPoints[0].State);
        Assert.Equal("sha256:new", initial.Preview.ContentHash);
        Assert.Equal(7, initial.Configuration.ExpectedContentVersion);

        await client.GetConfigurationAuthoringAsync(
            new ConfigurationAuthoringRequestModel
            {
                WorkspaceFolder = new ConfigurationWorkspaceFolderModel { Uri = "file:///workspace" },
                ExistingConfiguration = new ConfigurationExistingModel
                {
                    Content = "{}",
                    Version = 7,
                    ContentHash = "sha256:old",
                },
                Selections = new[]
                {
                    new ConfigurationSelectionModel
                    {
                        RelativePath = "Shaders/a.hlsl",
                        EntryPoint = "Main",
                        TargetProfile = "ps_6_6",
                    },
                },
                DraftContent = "{\"hlsl.variantsVersion\":1}",
            },
            CancellationToken.None);
        Assert.Equal("{}", capture.Request["existingConfiguration"]?["content"]?.Value<string>());
        Assert.Equal(7, capture.Request["existingConfiguration"]?["version"]?.Value<int>());
        Assert.Equal("ps_6_6", capture.Request["selections"]?[0]?["targetProfile"]?.Value<string>());
        Assert.Equal(
            "{\"hlsl.variantsVersion\":1}",
            capture.Request["draftContent"]?.Value<string>());
    }

    private sealed class AuthoringTarget
    {
        public JObject Request { get; private set; }

        [JsonRpcMethod(
            "hlsl/configurationAuthoring",
            UseSingleObjectParameterDeserialization = true)]
        public ConfigurationAuthoringResultModel Authoring(JObject request)
        {
            Request = request;
            return new ConfigurationAuthoringResultModel
            {
                ProtocolVersion = 1,
                Configuration = new ConfigurationFileModel
                {
                    Uri = "file:///workspace/shadertoolsconfig.json",
                    ExpectedContentVersion = 7,
                },
                Discovery = new ConfigurationDiscoveryModel
                {
                    Files = new[]
                    {
                        new ConfigurationDiscoveryFileModel
                        {
                            RelativePath = "Shaders/a.hlsl",
                            EntryPoints = new[]
                            {
                                new ConfigurationEntryPointModel
                                {
                                    Name = "Main",
                                    State = "ambiguous",
                                    TargetProfiles = new[] { "ps_6_6" },
                                },
                            },
                        },
                    },
                },
                Preview = new ConfigurationPreviewModel
                {
                    Content = "{}",
                    ContentHash = "sha256:new",
                    Valid = true,
                    Changed = true,
                },
            };
        }
    }
}
