using System;
using System.Threading;
using System.Threading.Tasks;
using HlslLsp.VisualStudio.Bootstrap;
using Nerdbank.Streams;
using Newtonsoft.Json;
using Newtonsoft.Json.Linq;
using StreamJsonRpc;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class ComputeVisualizationTests
{
    private const string SampleJson = @"
{
  ""applicable"": true,
  ""explanation"": """",
  ""entryPoint"": ""main"",
  ""stage"": ""compute"",
  ""targetProfile"": ""cs_6_6"",
  ""threadGroupSize"": { ""x"": 8, ""y"": 4, ""z"": 1 },
  ""dispatchDimensions"": { ""x"": 17, ""y"": 8, ""z"": 1 },
  ""groupCount"": { ""x"": 3, ""y"": 2, ""z"": 1 },
  ""launchedThreads"": 192,
  ""inactiveThreads"": 56,
  ""systemValues"": [{
    ""semantic"": ""SV_DispatchThreadID"",
    ""name"": ""dispatchThreadId"",
    ""formula"": ""groupId * numthreads + groupThreadId"",
    ""description"": ""Global dispatch-space thread coordinate.""
  }],
  ""barriers"": {
    ""available"": true,
    ""unavailableReason"": """",
    ""instructionCount"": 1,
    ""locations"": [{
      ""label"": ""GroupMemoryBarrierWithGroupSync"",
      ""uri"": ""file:///C:/shaders/compute.hlsl"",
      ""range"": {
        ""start"": { ""line"": 8, ""character"": 2 },
        ""end"": { ""line"": 8, ""character"": 34 }
      }
    }]
  },
  ""groupShared"": {
    ""available"": true,
    ""unavailableReason"": """",
    ""totalBytes"": 128,
    ""declarations"": [{
      ""name"": ""tile"",
      ""type"": ""float[32]"",
      ""sizeBytes"": 128,
      ""uri"": ""file:///C:/shaders/compute.hlsl"",
      ""range"": {
        ""start"": { ""line"": 1, ""character"": 0 },
        ""end"": { ""line"": 1, ""character"": 28 }
      }
    }]
  },
  ""waveSize"": {
    ""known"": true,
    ""min"": 32,
    ""max"": 64,
    ""preferred"": 32,
    ""explanation"": ""Wave-size attributes constrain this entry point.""
  },
  ""occupancy"": {
    ""hardwareProfile"": ""Test GPU"",
    ""estimatedResidentGroups"": 8,
    ""estimatedResidentThreads"": 256,
    ""estimatedResidentWaves"": 8,
    ""limitingFactors"": [""shared memory""],
    ""assumptions"": [""register pressure is unavailable""]
  }
}";

    [Fact]
    public void Deserialize_MapsCompleteResponse()
    {
        var model = JsonConvert.DeserializeObject<ComputeVisualizationModel>(SampleJson);

        Assert.True(model.Applicable);
        Assert.Equal("main", model.EntryPoint);
        Assert.Equal((uint)8, model.ThreadGroupSize.X);
        Assert.Equal((uint)3, model.GroupCount.X);
        Assert.Equal((ulong)192, model.LaunchedThreads);
        Assert.Equal((ulong)56, model.InactiveThreads);
        Assert.Equal("SV_DispatchThreadID", Assert.Single(model.SystemValues).Semantic);
        Assert.Equal((ulong)1, model.Barriers.InstructionCount);
        Assert.Equal(8, model.Barriers.Locations[0].Range.Start.Line);
        Assert.Equal((ulong)128, model.GroupShared.TotalBytes);
        Assert.Equal("tile", Assert.Single(model.GroupShared.Declarations).Name);
        Assert.True(model.WaveSize.Known);
        Assert.Equal((uint)32, model.WaveSize.Preferred);
        Assert.Equal("Test GPU", model.Occupancy.HardwareProfile);
        Assert.Equal((ulong)8, model.Occupancy.EstimatedResidentGroups);
    }

    [Fact]
    public void Deserialize_ToleratesAbsentForwardCompatibleSections()
    {
        var model = JsonConvert.DeserializeObject<ComputeVisualizationModel>(
            @"{ ""applicable"": false, ""explanation"": ""Not a compute shader."" }");

        Assert.False(model.Applicable);
        Assert.Equal("Not a compute shader.", model.Explanation);
        Assert.Null(model.ThreadGroupSize);
        Assert.Empty(model.SystemValues);
        Assert.Null(model.Barriers);
        Assert.Null(model.GroupShared);
        Assert.Null(model.WaveSize);
        Assert.Null(model.Occupancy);
    }

    [Fact]
    public void Display_LabelsGeometryUnknownSectionsAndOccupancyAsEstimate()
    {
        Assert.Equal(
            "8 x 4 x 1",
            ComputeVisualizationDisplay.Dimensions(
                new ComputeDimensionsModel { X = 8, Y = 4, Z = 1 }));
        Assert.Equal("Unavailable", ComputeVisualizationDisplay.Dimensions(null));
        Assert.Equal(
            "Hardware-dependent occupancy estimate",
            ComputeVisualizationDisplay.OccupancyHeading);
        Assert.Contains(
            "intentionally not guessed",
            ComputeVisualizationDisplay.OccupancyUnavailable(null));
        Assert.Equal(
            "Barrier analysis is unavailable.",
            ComputeVisualizationDisplay.UnavailableReason(
                null,
                "Barrier analysis is unavailable."));
        Assert.Contains("total threads/elements", ComputeVisualizationDisplay.LogicalWorkloadHelp);
        Assert.Contains("not D3D Dispatch()", ComputeVisualizationDisplay.LogicalWorkloadHelp);
        Assert.Contains("ceil", ComputeVisualizationDisplay.LogicalWorkloadHelp);
    }

    [Fact]
    public void Display_DistinguishesZeroBarriersFromUnavailableSourceLocations()
    {
        Assert.Equal(
            "(no barrier instructions)",
            ComputeVisualizationDisplay.BarrierLocationsMessage(0, 0));
        var unavailableLocations =
            ComputeVisualizationDisplay.BarrierLocationsMessage(2, 0);
        Assert.Contains("instructions were found", unavailableLocations);
        Assert.Contains("source locations are unavailable", unavailableLocations);
        Assert.Null(ComputeVisualizationDisplay.BarrierLocationsMessage(2, 2));
    }

    [Theory]
    [InlineData("1", 1u)]
    [InlineData("4294967295", uint.MaxValue)]
    public void Input_ParsesPositiveDispatchDimensions(string text, uint expected)
    {
        Assert.True(ComputeVisualizationInput.TryParsePositiveUInt32(text, out var actual));
        Assert.Equal(expected, actual);
    }

    [Theory]
    [InlineData("")]
    [InlineData("0")]
    [InlineData("-1")]
    [InlineData("1.5")]
    [InlineData(" 8")]
    [InlineData("4294967296")]
    public void Input_RejectsInvalidDispatchDimensions(string text)
        => Assert.False(ComputeVisualizationInput.TryParsePositiveUInt32(text, out _));

    [Fact]
    public async Task Bridge_ForwardsOptionsAndCancellation()
    {
        ComputeVisualizationOptions observedOptions = null;
        CancellationToken observedToken = default;
        ComputeVisualizationBridge.Register(
            (_, options, token) =>
            {
                observedOptions = options;
                observedToken = token;
                return Task.FromResult(new ComputeVisualizationModel { Applicable = true });
            });
        using var cancellation = new CancellationTokenSource();
        var expectedOptions = new ComputeVisualizationOptions
        {
            DispatchDimensions = new ComputeDimensionsModel { X = 2, Y = 3, Z = 4 },
        };

        var result = await ComputeVisualizationBridge.RequestAsync(
            new Uri("file:///C:/shaders/compute.hlsl"),
            expectedOptions,
            cancellation.Token);

        Assert.True(result.Applicable);
        Assert.Same(expectedOptions, observedOptions);
        Assert.Equal(cancellation.Token, observedToken);
    }

    [Fact]
    public void Refresh_PreservesCurrentInteractiveOptions()
    {
        var current = new ComputeVisualizationOptions
        {
            DispatchDimensions = new ComputeDimensionsModel { X = 1920, Y = 1080, Z = 1 },
            HardwareProfile = new ComputeHardwareProfileModel { Name = "Test GPU", WaveSize = 32 },
        };

        Assert.Same(
            current,
            ComputeVisualizationRefreshLogic.OptionsForBackgroundRefresh(current));
        var uri = new Uri("file:///C:/shaders/compute.hlsl");
        Assert.True(
            ComputeVisualizationRefreshLogic.ShouldPreserveContentOnFailure(uri, uri));
        Assert.False(
            ComputeVisualizationRefreshLogic.ShouldRevealToolWindow(
                existingWindowSupplied: true));
    }

    [Fact]
    public void Refresh_DefersBackgroundWorkDuringInteractiveApply()
    {
        var gate = new EntryPointDataFlowRefreshGate();

        gate.EnterExplicitRequest();
        Assert.False(gate.TryBeginBackgroundRefresh());
        Assert.True(gate.ExitExplicitRequest());
        Assert.True(gate.TryBeginBackgroundRefresh());
    }

    [Fact]
    public async Task Request_SendsOnlyConfiguredOptionalInputs()
    {
        var (serverStream, clientStream) = FullDuplexStream.CreatePair();
        var target = new FakeComputeServer();
        using var serverRpc = new JsonRpc(serverStream, serverStream, target);
        var client = new HlslLanguageClient(
            "2021",
            string.Empty,
            string.Empty,
            (_, _) => Task.CompletedTask,
            _ => Task.CompletedTask);
        using var clientRpc = new JsonRpc(clientStream, clientStream, client.CustomMessageTarget);
        serverRpc.StartListening();
        clientRpc.StartListening();
        await client.AttachForCustomMessageAsync(clientRpc);

        await client.GetComputeVisualizationAsync(
            new Uri("file:///C:/shaders/compute.hlsl"),
            new ComputeVisualizationOptions(),
            CancellationToken.None);

        Assert.Equal(
            "file:///C:/shaders/compute.hlsl",
            target.Received.SelectToken("textDocument.uri")?.Value<string>());
        Assert.Null(target.Received["dispatchDimensions"]);
        Assert.Null(target.Received["hardwareProfile"]);

        await client.GetComputeVisualizationAsync(
            new Uri("file:///C:/shaders/compute.hlsl"),
            new ComputeVisualizationOptions
            {
                DispatchDimensions = new ComputeDimensionsModel { X = 10, Y = 20, Z = 1 },
                HardwareProfile = new ComputeHardwareProfileModel
                {
                    Name = "Test GPU",
                    WaveSize = 32,
                    MaxThreadsPerGroup = 1024,
                    MaxThreadsPerComputeUnit = 2048,
                    MaxGroupsPerComputeUnit = 32,
                    SharedMemoryBytesPerComputeUnit = 65536,
                },
            },
            CancellationToken.None);

        Assert.Equal((uint)10, target.Received.SelectToken("dispatchDimensions.x")?.Value<uint>());
        Assert.Equal("Test GPU", target.Received.SelectToken("hardwareProfile.name")?.Value<string>());
        Assert.Equal(
            (ulong)65536,
            target.Received
                .SelectToken("hardwareProfile.sharedMemoryBytesPerComputeUnit")
                ?.Value<ulong>());
    }

    private sealed class FakeComputeServer
    {
        internal JObject Received { get; private set; }

        [JsonRpcMethod(
            "hlsl/computeVisualization",
            UseSingleObjectParameterDeserialization = true)]
        public ComputeVisualizationModel Handle(JObject parameters)
        {
            Received = parameters;
            return JsonConvert.DeserializeObject<ComputeVisualizationModel>(SampleJson);
        }
    }

}
