using HlslLsp.VisualStudio.Bootstrap;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class DisassemblyFileNamingTests
{
    [Theory]
    [InlineData(@"C:\shaders\lighting.hlsl", "dxil", "lighting.ll")]
    [InlineData(@"C:\shaders\lighting.hlsl", "spirv", "lighting.spvasm")]
    [InlineData(@"C:\shaders\lighting.hlsl", "SPIRV", "lighting.spvasm")]
    [InlineData(@"C:\shaders\post.fx.hlsl", "dxil", "post.fx.ll")]
    public void SuggestedFileName_KeepsDocumentBaseNameAndSwapsExtension(
        string documentPath,
        string format,
        string expected)
    {
        Assert.Equal(
            expected,
            DisassemblyFileNaming.SuggestedFileName(documentPath, format));
    }

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    public void SuggestedFileName_FallsBackToGenericNameForAnUnnamedDocument(
        string documentPath)
    {
        Assert.Equal(
            "shader.ll",
            DisassemblyFileNaming.SuggestedFileName(documentPath, "dxil"));
    }

    [Fact]
    public void SuggestedFileName_FallsBackWhenPathHasNoFileNameComponent()
    {
        Assert.Equal(
            "shader.ll",
            DisassemblyFileNaming.SuggestedFileName(@"C:\shaders\", "dxil"));
    }

    [Fact]
    public void SuggestedFileName_FallsBackWhenPathContainsInvalidFileNameCharacters()
    {
        // A path built from a non-file document uri (for example, an
        // untitled/virtual buffer) may contain characters that are invalid
        // for a Windows file name; Path.GetFileNameWithoutExtension throws
        // ArgumentException for those, so this must fall back to the
        // generic default rather than propagate that exception.
        Assert.Equal(
            "shader.spvasm",
            DisassemblyFileNaming.SuggestedFileName("untitled:Untitled-1*|<>", "spirv"));
    }

    [Fact]
    public void SavedContentUsesUtf8WithoutAByteOrderMark()
    {
        var bytes = DisassemblyFileContent.Encode("; DXIL\n");

        Assert.Equal((byte)';', bytes[0]);
        Assert.Equal("; DXIL\n", System.Text.Encoding.UTF8.GetString(bytes));
    }
}
