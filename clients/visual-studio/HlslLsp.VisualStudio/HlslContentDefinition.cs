using System.ComponentModel.Composition;
using Microsoft.VisualStudio.LanguageServer.Client;
using Microsoft.VisualStudio.Utilities;

namespace HlslLsp.VisualStudio;

internal static class HlslContentDefinition
{
    internal const string ContentTypeName = "HLSL-LSP";

#pragma warning disable CS0649
    [Export]
    [Name(ContentTypeName)]
    [BaseDefinition("HLSL")]
    [BaseDefinition(CodeRemoteContentDefinition.CodeRemoteContentTypeName)]
    internal static ContentTypeDefinition HlslContentType;

    [Export]
    [FileExtension(".hlsl")]
    [ContentType(ContentTypeName)]
    internal static FileExtensionToContentTypeDefinition HlslFileExtension;

    [Export]
    [FileExtension(".hlsli")]
    [ContentType(ContentTypeName)]
    internal static FileExtensionToContentTypeDefinition HlslIncludeFileExtension;
#pragma warning restore CS0649
}
