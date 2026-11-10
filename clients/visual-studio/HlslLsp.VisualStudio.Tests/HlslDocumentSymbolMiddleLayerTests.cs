using System;
using System.IO;
using System.Threading.Tasks;
using Microsoft.VisualStudio.LanguageServer.Client;
using Newtonsoft.Json.Linq;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class HlslDocumentSymbolMiddleLayerTests
{
    private const string Method = "textDocument/documentSymbol";
    private const string Uri = "file:///C:/shader.comp";

    [Fact]
    public async Task FlattensHierarchicalSymbolsForNativeNavigation()
    {
        var client = CreateClient();
        var layer = Assert.IsAssignableFrom<ILanguageClientMiddleLayer2<JToken>>(client.MiddleLayer);
        Assert.True(layer.CanHandle(Method));
        Assert.False(layer.CanHandle("textDocument/didOpen"));
        Assert.False(layer.CanHandle("textDocument/definition"));

        var request = JObject.Parse(
            "{\"textDocument\":{\"uri\":\"file:///C:/shader.comp\"}}");
        var symbols = JArray.Parse(@"[
            {
                'name': 'Outer', 'kind': 23,
                'range': { 'start': { 'line': 1, 'character': 0 }, 'end': { 'line': 8, 'character': 1 } },
                'selectionRange': { 'start': { 'line': 1, 'character': 7 }, 'end': { 'line': 1, 'character': 12 } },
                'tags': [1], 'deprecated': true,
                'children': [
                    {
                        'name': 'member', 'kind': 8,
                        'range': { 'start': { 'line': 2, 'character': 0 }, 'end': { 'line': 2, 'character': 13 } },
                        'selectionRange': { 'start': { 'line': 2, 'character': 6 }, 'end': { 'line': 2, 'character': 12 } }
                    },
                    {
                        'name': 'Inner', 'kind': 23,
                        'range': { 'start': { 'line': 3, 'character': 0 }, 'end': { 'line': 7, 'character': 1 } },
                        'selectionRange': { 'start': { 'line': 3, 'character': 7 }, 'end': { 'line': 3, 'character': 12 } },
                        'children': [{
                            'name': 'leaf', 'kind': 8,
                            'range': { 'start': { 'line': 4, 'character': 0 }, 'end': { 'line': 4, 'character': 11 } },
                            'selectionRange': { 'start': { 'line': 4, 'character': 6 }, 'end': { 'line': 4, 'character': 10 } }
                        }]
                    }
                ]
            },
            {
                'name': 'main', 'kind': 12,
                'range': { 'start': { 'line': 9, 'character': 0 }, 'end': { 'line': 10, 'character': 1 } },
                'selectionRange': { 'start': { 'line': 9, 'character': 7 }, 'end': { 'line': 9, 'character': 11 } }
            }
        ]");
        var original = symbols.DeepClone();
        var calls = 0;
        var result = await layer.HandleRequestAsync(Method, request, parameters =>
        {
            Assert.Same(request, parameters);
            ++calls;
            return Task.FromResult<JToken>(symbols);
        });

        var expected = JArray.Parse(@"[
            {
                'name': 'Outer', 'kind': 23,
                'location': { 'uri': 'file:///C:/shader.comp', 'range': { 'start': { 'line': 1, 'character': 7 }, 'end': { 'line': 1, 'character': 12 } } },
                'tags': [1], 'deprecated': true
            },
            {
                'name': 'member', 'kind': 8, 'containerName': 'Outer',
                'location': { 'uri': 'file:///C:/shader.comp', 'range': { 'start': { 'line': 2, 'character': 6 }, 'end': { 'line': 2, 'character': 12 } } }
            },
            {
                'name': 'Inner', 'kind': 23, 'containerName': 'Outer',
                'location': { 'uri': 'file:///C:/shader.comp', 'range': { 'start': { 'line': 3, 'character': 7 }, 'end': { 'line': 3, 'character': 12 } } }
            },
            {
                'name': 'leaf', 'kind': 8, 'containerName': 'Outer::Inner',
                'location': { 'uri': 'file:///C:/shader.comp', 'range': { 'start': { 'line': 4, 'character': 6 }, 'end': { 'line': 4, 'character': 10 } } }
            },
            {
                'name': 'main', 'kind': 12,
                'location': { 'uri': 'file:///C:/shader.comp', 'range': { 'start': { 'line': 9, 'character': 7 }, 'end': { 'line': 9, 'character': 11 } } }
            }
        ]");
        Assert.Equal(1, calls);
        Assert.True(JToken.DeepEquals(expected, result));
        Assert.True(JToken.DeepEquals(original, symbols));
    }

    [Fact]
    public async Task OverloadsRemainSeparateNativeNavigationTargets()
    {
        var layer = new HlslDocumentSymbolMiddleLayer();
        var request = new JObject { ["textDocument"] = new JObject { ["uri"] = Uri } };
        var symbols = JArray.Parse(@"[
            {
                'name': 'work', 'kind': 12,
                'range': { 'start': { 'line': 1, 'character': 0 }, 'end': { 'line': 4, 'character': 1 } },
                'selectionRange': { 'start': { 'line': 1, 'character': 6 }, 'end': { 'line': 1, 'character': 10 } }
            },
            {
                'name': 'work', 'kind': 12,
                'range': { 'start': { 'line': 6, 'character': 0 }, 'end': { 'line': 9, 'character': 1 } },
                'selectionRange': { 'start': { 'line': 6, 'character': 6 }, 'end': { 'line': 6, 'character': 10 } }
            }
        ]");
        var result = Assert.IsType<JArray>(await layer.HandleRequestAsync(
            Method, request, _ => Task.FromResult<JToken>(symbols)));
        Assert.Equal(2, result.Count);
        Assert.Equal("work", (string)result[0]["name"]);
        Assert.Equal("work", (string)result[1]["name"]);
        Assert.Equal(1, (int)result[0]["location"]["range"]["start"]["line"]);
        Assert.Equal(6, (int)result[1]["location"]["range"]["start"]["line"]);
    }

    [Fact]
    public async Task PreservesFlatNullAndEmptyResponses()
    {
        var layer = new HlslDocumentSymbolMiddleLayer();
        var flat = JArray.Parse(@"[
            {
                'name': 'main', 'kind': 12, 'containerName': 'Global',
                'location': { 'uri': 'file:///C:/other.hlsl', 'range': { 'start': { 'line': 0, 'character': 1 }, 'end': { 'line': 0, 'character': 5 } } }
            }
        ]");
        foreach (var response in new JToken[] { flat, null, JValue.CreateNull(), new JArray() })
        {
            var actual = await layer.HandleRequestAsync(
                Method, null, _ => Task.FromResult(response));
            Assert.Same(response, actual);
        }
    }

    [Theory]
    [InlineData("{}")]
    [InlineData("[1]")]
    [InlineData("[{'name':'bad','kind':12}]")]
    [InlineData("[{'name':'bad','kind':12,'selectionRange':{}}]")]
    [InlineData("[{'name':'bad','kind':12,'selectionRange':{'start':{'line':-1,'character':0},'end':{'line':0,'character':1}}}]")]
    [InlineData("[{'name':'bad','kind':12,'selectionRange':{'start':{'line':0,'character':0},'end':{'line':0,'character':1}},'children':{}}]")]
    [InlineData("[{'name':'flat','kind':12,'location':{}},{'name':'tree','kind':12,'selectionRange':{}}]")]
    public async Task RejectsMalformedOrMixedResponses(string json)
    {
        var layer = new HlslDocumentSymbolMiddleLayer();
        var request = new JObject { ["textDocument"] = new JObject { ["uri"] = Uri } };
        await Assert.ThrowsAsync<InvalidDataException>(() =>
            layer.HandleRequestAsync(
                Method, request, _ => Task.FromResult(JToken.Parse(json))));
    }

    [Fact]
    public async Task RequiresDocumentUriForHierarchicalResults()
    {
        var layer = new HlslDocumentSymbolMiddleLayer();
        var response = JArray.Parse("[{'name':'main','kind':12,'selectionRange':{}}]");
        await Assert.ThrowsAsync<InvalidDataException>(() =>
            layer.HandleRequestAsync(Method, null, _ => Task.FromResult<JToken>(response)));
    }

    [Fact]
    public async Task PropagatesTransportErrorsAndForwardsOtherMessages()
    {
        var layer = new HlslDocumentSymbolMiddleLayer();
        var error = new InvalidOperationException("Transport failure.");
        Assert.Same(error, await Assert.ThrowsAsync<InvalidOperationException>(() =>
            layer.HandleRequestAsync(Method, null, _ => Task.FromException<JToken>(error))));

        var parameters = new JObject();
        var response = new JObject();
        Assert.Same(response, await layer.HandleRequestAsync(
            "textDocument/definition", parameters, forwarded =>
            {
                Assert.Same(parameters, forwarded);
                return Task.FromResult<JToken>(response);
            }));
        await layer.HandleNotificationAsync("textDocument/didOpen", parameters, forwarded =>
        {
            Assert.Same(parameters, forwarded);
            return Task.CompletedTask;
        });
    }

    private static HlslLanguageClient CreateClient() => new(
        "2021", string.Empty, string.Empty,
        (_, _) => Task.CompletedTask, _ => Task.CompletedTask);
}
