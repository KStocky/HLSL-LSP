using System.ComponentModel.Composition;
using Microsoft.VisualStudio.Utilities;

namespace HlslLsp.VisualStudio.Bootstrap;

internal static class HlslContentDefinitions
{
#pragma warning disable CS0649
    [Export(typeof(ContentTypeDefinition))]
    [Name("HLSL")]
    [BaseDefinition("code")]
    internal static ContentTypeDefinition ShaderContentType;

    [Export(typeof(ContentTypeDefinition))]
    [Name("HLSLHeader")]
    [BaseDefinition("HLSL")]
    internal static ContentTypeDefinition HeaderContentType;

    [Export(typeof(FileExtensionToContentTypeDefinition))]
    [FileExtension(".hlsl")]
    [ContentType("HLSL")]
    internal static FileExtensionToContentTypeDefinition ShaderExtension;

    [Export(typeof(FileExtensionToContentTypeDefinition))]
    [FileExtension(".hlsli")]
    [ContentType("HLSLHeader")]
    internal static FileExtensionToContentTypeDefinition HeaderExtension;
#pragma warning restore CS0649
}
