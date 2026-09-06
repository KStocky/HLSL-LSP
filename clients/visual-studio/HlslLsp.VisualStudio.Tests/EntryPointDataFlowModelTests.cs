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

// Model/deserialization coverage for the hlsl/entryPointDataFlow protocol
// (see docs/call-hierarchy.md): every field the server can report is
// modelled type-safely, so this deserializes the exact response shape the
// docs describe (including the nested opaque CallHierarchyItem.data
// envelope) and asserts every field lands in the right typed property.
public sealed class EntryPointDataFlowModelTests
{
    // Matches docs/call-hierarchy.md's "Response shape" sample for
    // hlsl/entryPointDataFlow verbatim.
    private const string SampleJson = @"
{
  ""found"": true,
  ""explanation"": """",
  ""entryPoint"": {
    ""name"": ""square"",
    ""kind"": 12,
    ""detail"": ""float square(float x)"",
    ""uri"": ""file:///C:/shaders/example.hlsl"",
    ""range"": { ""start"": { ""line"": 3, ""character"": 0 }, ""end"": { ""line"": 3, ""character"": 40 } },
    ""selectionRange"": { ""start"": { ""line"": 3, ""character"": 6 }, ""end"": { ""line"": 3, ""character"": 12 } },
    ""data"": {
      ""rootUri"": ""file:///C:/shaders/example.hlsl"",
      ""rootIdentity"": ""abc"",
      ""rootVersion"": 1,
      ""path"": ""C:/shaders/example.hlsl"",
      ""line"": 4,
      ""column"": 7,
      ""startOffset"": 87,
      ""cursorKind"": 21,
      ""name"": ""square""
    }
  },
  ""reachableFunctions"": [
    {
      ""function"": {
        ""name"": ""square"",
        ""kind"": 12,
        ""detail"": ""float square(float x)"",
        ""uri"": ""file:///C:/shaders/example.hlsl"",
        ""range"": { ""start"": { ""line"": 3, ""character"": 0 }, ""end"": { ""line"": 3, ""character"": 40 } },
        ""selectionRange"": { ""start"": { ""line"": 3, ""character"": 6 }, ""end"": { ""line"": 3, ""character"": 12 } },
        ""data"": { ""rootUri"": ""file:///C:/shaders/example.hlsl"" }
      },
      ""depth"": 0,
      ""recursive"": false
    }
  ],
  ""unreachableFunctions"": [],
  ""unusedDeclarations"": [
    {
      ""name"": ""unusedHelper"",
      ""kind"": 12,
      ""uri"": ""file:///C:/shaders/example.hlsl"",
      ""range"": { ""start"": { ""line"": 10, ""character"": 0 }, ""end"": { ""line"": 12, ""character"": 1 } },
      ""selectionRange"": { ""start"": { ""line"": 10, ""character"": 6 }, ""end"": { ""line"": 10, ""character"": 17 } }
    }
  ],
  ""globalAccesses"": [
    {
      ""name"": ""InputTexture"",
      ""kind"": 268,
      ""uri"": ""file:///C:/shaders/example.hlsl"",
      ""range"": { ""start"": { ""line"": 1, ""character"": 0 }, ""end"": { ""line"": 1, ""character"": 34 } },
      ""selectionRange"": { ""start"": { ""line"": 1, ""character"": 20 }, ""end"": { ""line"": 1, ""character"": 32 } },
      ""qualifiedName"": ""InputTexture"",
      ""access"": ""read""
    }
  ],
  ""truncated"": false,
  ""functionsVisitedTruncated"": false,
  ""definitionsTruncated"": false,
  ""globalAccessesTruncated"": false,
  ""unusedDeclarationsTruncated"": false,
  ""functionsVisited"": 4
}";

    [Fact]
    public void Deserialize_MapsEveryDocumentedFieldToItsTypedProperty()
    {
        var model = JsonConvert.DeserializeObject<EntryPointDataFlowModel>(SampleJson);

        Assert.True(model.Found);
        Assert.Equal(string.Empty, model.Explanation);
        Assert.False(model.Truncated);
        Assert.False(model.FunctionsVisitedTruncated);
        Assert.False(model.DefinitionsTruncated);
        Assert.False(model.GlobalAccessesTruncated);
        Assert.False(model.UnusedDeclarationsTruncated);
        Assert.Equal(4, model.FunctionsVisited);

        Assert.NotNull(model.EntryPoint);
        Assert.Equal("square", model.EntryPoint.Name);
        Assert.Equal(12, model.EntryPoint.Kind);
        Assert.Equal("float square(float x)", model.EntryPoint.Detail);
        Assert.Equal("file:///C:/shaders/example.hlsl", model.EntryPoint.Uri);
        Assert.Equal(3, model.EntryPoint.Range.Start.Line);
        Assert.Equal(0, model.EntryPoint.Range.Start.Character);
        Assert.Equal(3, model.EntryPoint.Range.End.Line);
        Assert.Equal(40, model.EntryPoint.Range.End.Character);
        Assert.Equal(6, model.EntryPoint.SelectionRange.Start.Character);
        Assert.IsAssignableFrom<JToken>(model.EntryPoint.Data);
        Assert.Equal("abc", model.EntryPoint.Data["rootIdentity"]?.Value<string>());
        Assert.Equal(1, model.EntryPoint.Data["rootVersion"]?.Value<int>());

        Assert.Single(model.ReachableFunctions);
        var reachable = model.ReachableFunctions[0];
        Assert.Equal("square", reachable.Function.Name);
        Assert.Equal(0, reachable.Depth);
        Assert.False(reachable.Recursive);

        Assert.Empty(model.UnreachableFunctions);

        Assert.Single(model.UnusedDeclarations);
        var unused = model.UnusedDeclarations[0];
        Assert.Equal("unusedHelper", unused.Name);
        Assert.Equal(12, unused.Kind);
        Assert.Equal(10, unused.Range.Start.Line);
        Assert.Equal(12, unused.Range.End.Line);

        Assert.Single(model.GlobalAccesses);
        var access = model.GlobalAccesses[0];
        Assert.Equal("InputTexture", access.Name);
        Assert.Equal(268, access.Kind);
        Assert.Equal("InputTexture", access.QualifiedName);
        Assert.Equal("read", access.Access);
        Assert.Equal(1, access.Range.Start.Line);
    }

    [Fact]
    public void Deserialize_NotFoundResponseLeavesEntryPointNullAndListsEmpty()
    {
        const string json = @"
{
  ""found"": false,
  ""explanation"": ""No entry point is configured."",
  ""entryPoint"": null,
  ""reachableFunctions"": [],
  ""unreachableFunctions"": [],
  ""unusedDeclarations"": [],
  ""globalAccesses"": [],
  ""truncated"": false,
  ""functionsVisitedTruncated"": false,
  ""definitionsTruncated"": false,
  ""globalAccessesTruncated"": false,
  ""unusedDeclarationsTruncated"": false,
  ""functionsVisited"": 0
}";

        var model = JsonConvert.DeserializeObject<EntryPointDataFlowModel>(json);

        Assert.False(model.Found);
        Assert.Equal("No entry point is configured.", model.Explanation);
        Assert.Null(model.EntryPoint);
        Assert.Empty(model.ReachableFunctions);
        Assert.Empty(model.UnreachableFunctions);
        Assert.Empty(model.UnusedDeclarations);
        Assert.Empty(model.GlobalAccesses);
    }

    [Fact]
    public void Deserialize_TruncatedResponseReportsBoundedTraversal()
    {
        const string json = @"
{
  ""found"": true,
  ""explanation"": """",
  ""entryPoint"": null,
  ""reachableFunctions"": [],
  ""unreachableFunctions"": [],
  ""unusedDeclarations"": [],
  ""globalAccesses"": [],
  ""truncated"": true,
  ""functionsVisitedTruncated"": true,
  ""definitionsTruncated"": true,
  ""globalAccessesTruncated"": true,
  ""unusedDeclarationsTruncated"": true,
  ""functionsVisited"": 4096
}";

        var model = JsonConvert.DeserializeObject<EntryPointDataFlowModel>(json);

        Assert.True(model.Truncated);
        Assert.True(model.FunctionsVisitedTruncated);
        Assert.True(model.DefinitionsTruncated);
        Assert.True(model.GlobalAccessesTruncated);
        Assert.True(model.UnusedDeclarationsTruncated);
        Assert.Equal(4096, model.FunctionsVisited);
    }

    // The four section-specific flags are independent causes (see
    // docs/call-hierarchy.md): a response can have only the global-access
    // scan truncated while the reachability traversal, definition
    // collection, and unused-declaration scan all completed. The client
    // must be able to distinguish this from "the whole thing was
    // truncated" so it never falsely blames the function graph
    // (Unreachable functions) for an unrelated section's budget being hit.
    [Fact]
    public void Deserialize_TruncationFlagsAreIndependentPerSection()
    {
        const string json = @"
{
  ""found"": true,
  ""explanation"": """",
  ""entryPoint"": null,
  ""reachableFunctions"": [],
  ""unreachableFunctions"": [],
  ""unusedDeclarations"": [],
  ""globalAccesses"": [],
  ""truncated"": true,
  ""functionsVisitedTruncated"": false,
  ""definitionsTruncated"": false,
  ""globalAccessesTruncated"": true,
  ""unusedDeclarationsTruncated"": false,
  ""functionsVisited"": 12
}";

        var model = JsonConvert.DeserializeObject<EntryPointDataFlowModel>(json);

        Assert.True(model.Truncated);
        Assert.False(model.FunctionsVisitedTruncated);
        Assert.False(model.DefinitionsTruncated);
        Assert.True(model.GlobalAccessesTruncated);
        Assert.False(model.UnusedDeclarationsTruncated);
    }

    // DefinitionsTruncated is independent of FunctionsVisitedTruncated: a
    // translation unit can have a tiny, fully-explored reachable call graph
    // (traversal not truncated) while still having an enormous number of
    // unrelated dead function definitions (definition collection
    // truncated). Both are documented to leave UnreachableFunctions
    // completely empty for the same underlying reason (an unclassified
    // definition cannot be told apart from a classified-reachable one), but
    // they are still distinct, independently reportable causes.
    [Fact]
    public void Deserialize_DefinitionsTruncatedIsIndependentOfFunctionsVisitedTruncated()
    {
        const string json = @"
{
  ""found"": true,
  ""explanation"": """",
  ""entryPoint"": null,
  ""reachableFunctions"": [],
  ""unreachableFunctions"": [],
  ""unusedDeclarations"": [],
  ""globalAccesses"": [],
  ""truncated"": true,
  ""functionsVisitedTruncated"": false,
  ""definitionsTruncated"": true,
  ""globalAccessesTruncated"": false,
  ""unusedDeclarationsTruncated"": false,
  ""functionsVisited"": 4
}";

        var model = JsonConvert.DeserializeObject<EntryPointDataFlowModel>(json);

        Assert.True(model.Truncated);
        Assert.False(model.FunctionsVisitedTruncated);
        Assert.True(model.DefinitionsTruncated);
        Assert.False(model.GlobalAccessesTruncated);
        Assert.False(model.UnusedDeclarationsTruncated);
    }

    // A response predating this field (or one the server omits it from)
    // must not be treated as truncated by default -- absence must mean
    // "not truncated", matching every other bool flag's safe default.
    [Fact]
    public void Deserialize_DefinitionsTruncatedDefaultsToFalseWhenFieldIsAbsent()
    {
        const string json = @"
{
  ""found"": true,
  ""explanation"": """",
  ""entryPoint"": null,
  ""reachableFunctions"": [],
  ""unreachableFunctions"": [],
  ""unusedDeclarations"": [],
  ""globalAccesses"": [],
  ""truncated"": false,
  ""functionsVisitedTruncated"": false,
  ""globalAccessesTruncated"": false,
  ""unusedDeclarationsTruncated"": false,
  ""functionsVisited"": 0
}";

        var model = JsonConvert.DeserializeObject<EntryPointDataFlowModel>(json);

        Assert.False(model.DefinitionsTruncated);
    }

    // Direct coverage of the bridge itself (mirrors the decoupling
    // CompilationInfoBridge/PreprocessorExplorerBridge already use): a
    // registered handler is invoked with the exact uri/token supplied, and
    // an unregistered bridge resolves to null rather than throwing so a
    // package built without an activated language client degrades safely.
    [Fact]
    public async Task Bridge_ForwardsUriAndCancellationTokenToRegisteredHandler()
    {
        Uri observedUri = null;
        CancellationToken observedToken = default;
        var expected = new EntryPointDataFlowModel { Found = true, Explanation = "ok" };
        EntryPointDataFlowBridge.Register(
            (uri, token) =>
            {
                observedUri = uri;
                observedToken = token;
                return Task.FromResult(expected);
            });

        using var cancellation = new CancellationTokenSource();
        var requestUri = new Uri("file:///C:/shaders/example.hlsl");
        var result = await EntryPointDataFlowBridge.RequestAsync(requestUri, cancellation.Token);

        Assert.Same(expected, result);
        Assert.Equal(requestUri, observedUri);
        Assert.Equal(cancellation.Token, observedToken);
    }

    [Fact]
    public void Bridge_Register_RejectsNullHandler()
        => Assert.Throws<ArgumentNullException>(() => EntryPointDataFlowBridge.Register(null));

    // Exercises the real StreamJsonRpc wiring HlslLanguageClient relies on
    // for this request, mirroring HlslLanguageClientCustomMessageTests: a
    // fake server target answers hlsl/entryPointDataFlow with the
    // documented response shape, and this drives it through the exact
    // production entry point (HlslLanguageClient.GetEntryPointDataFlowAsync,
    // via the real AttachForCustomMessageAsync wiring), asserting both the
    // documented { textDocument: { uri } } params and correct response
    // deserialization into EntryPointDataFlowModel -- concrete
    // protocol-level evidence rather than an assumption about parameter
    // binding.
    [Fact]
    public async Task GetEntryPointDataFlowAsync_SendsDocumentUriAndDeserializesResponse()
    {
        var (serverStream, clientStream) = FullDuplexStream.CreatePair();
        var target = new FakeEntryPointDataFlowServer();
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

        var result = await client.GetEntryPointDataFlowAsync(
            new Uri("file:///C:/shaders/example.hlsl"),
            CancellationToken.None);

        Assert.NotNull(target.ReceivedParams);
        Assert.Equal(
            "file:///C:/shaders/example.hlsl",
            target.ReceivedParams.TextDocument?.Uri);
        Assert.True(result.Found);
        Assert.Equal("square", result.EntryPoint.Name);
        Assert.Single(result.ReachableFunctions);
        Assert.Equal("InputTexture", result.GlobalAccesses[0].Name);
        Assert.Equal("read", result.GlobalAccesses[0].Access);

        serverStream.Dispose();
        clientStream.Dispose();
    }

    private sealed class FakeEntryPointDataFlowServer
    {
        internal FakeTextDocumentParams ReceivedParams { get; private set; }

        [JsonRpcMethod(
            "hlsl/entryPointDataFlow",
            UseSingleObjectParameterDeserialization = true)]
        public EntryPointDataFlowModel EntryPointDataFlow(FakeTextDocumentParams parameters)
        {
            ReceivedParams = parameters;
            return JsonConvert.DeserializeObject<EntryPointDataFlowModel>(SampleJson);
        }
    }

    private sealed class FakeTextDocumentParams
    {
        public FakeTextDocumentIdentifier TextDocument { get; set; }
    }

    private sealed class FakeTextDocumentIdentifier
    {
        public string Uri { get; set; }
    }
}
