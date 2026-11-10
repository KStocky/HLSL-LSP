using System;
using System.IO;
using System.Threading.Tasks;
using Microsoft.VisualStudio.LanguageServer.Client;
using Newtonsoft.Json.Linq;

namespace HlslLsp.VisualStudio;

internal sealed class HlslDocumentSymbolMiddleLayer : ILanguageClientMiddleLayer2<JToken>
{
    private const string DocumentSymbolMethod = "textDocument/documentSymbol";

    public bool CanHandle(string methodName) =>
        string.Equals(methodName, DocumentSymbolMethod, StringComparison.Ordinal);

    public Task HandleNotificationAsync(
        string methodName, JToken methodParam, Func<JToken, Task> sendNotification)
        => sendNotification(methodParam);

    public async Task<JToken> HandleRequestAsync(
        string methodName, JToken methodParam, Func<JToken, Task<JToken>> sendRequest)
    {
        var response = await sendRequest(methodParam).ConfigureAwait(false);
        if (!CanHandle(methodName) || response == null || response.Type == JTokenType.Null)
        {
            return response;
        }
        if (response is not JArray symbols)
        {
            throw new InvalidDataException("The document symbol response is not an array.");
        }
        if (symbols.Count == 0)
        {
            return response;
        }
        if (symbols[0] is not JObject first)
        {
            throw new InvalidDataException("The document symbol response contains an invalid symbol.");
        }
        if (first["location"] is JObject)
        {
            foreach (var symbol in symbols)
            {
                if (symbol is not JObject item || item["location"] is not JObject)
                {
                    throw new InvalidDataException("The document symbol response mixes symbol formats.");
                }
            }
            return response;
        }

        var uri = methodParam?["textDocument"]?["uri"];
        if (uri?.Type != JTokenType.String || string.IsNullOrWhiteSpace((string)uri))
        {
            throw new InvalidDataException("The document symbol request has no document URI.");
        }

        // Visual Studio's native navigation bar requests SymbolInformation[],
        // but the server returns hierarchical DocumentSymbol values.
        var flattened = new JArray();
        foreach (var symbol in symbols)
        {
            AppendSymbol(flattened, symbol, (string)uri, null);
        }
        return flattened;
    }

    private static void AppendSymbol(
        JArray result, JToken value, string uri, string containerName)
    {
        if (value is not JObject symbol ||
            symbol["location"] != null ||
            symbol["name"]?.Type != JTokenType.String ||
            symbol["kind"]?.Type != JTokenType.Integer ||
            symbol["selectionRange"] is not JObject selectionRange ||
            !IsPosition(selectionRange["start"]) ||
            !IsPosition(selectionRange["end"]))
        {
            throw new InvalidDataException("The document symbol response contains an invalid symbol.");
        }

        var name = (string)symbol["name"];
        var item = new JObject
        {
            ["name"] = name,
            ["kind"] = symbol["kind"].DeepClone(),
            ["location"] = new JObject
            {
                ["uri"] = uri,
                ["range"] = selectionRange.DeepClone(),
            },
        };
        if (containerName != null)
        {
            item["containerName"] = containerName;
        }
        foreach (var property in new[] { "tags", "deprecated" })
        {
            if (symbol[property] != null)
            {
                item[property] = symbol[property].DeepClone();
            }
        }
        result.Add(item);

        var children = symbol["children"];
        if (children == null || children.Type == JTokenType.Null)
        {
            return;
        }
        if (children is not JArray childSymbols)
        {
            throw new InvalidDataException("The document symbol children are not an array.");
        }
        var childContainer = containerName == null ? name : containerName + "::" + name;
        foreach (var child in childSymbols)
        {
            AppendSymbol(result, child, uri, childContainer);
        }
    }

    private static bool IsPosition(JToken value) =>
        value is JObject position &&
        position["line"]?.Type == JTokenType.Integer &&
        position["character"]?.Type == JTokenType.Integer &&
        (long)position["line"] >= 0 &&
        (long)position["character"] >= 0;
}
