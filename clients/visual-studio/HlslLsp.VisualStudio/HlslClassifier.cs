using System;
using System.Collections.Generic;
using System.ComponentModel.Composition;
using Microsoft.VisualStudio.Language.StandardClassification;
using Microsoft.VisualStudio.Text;
using Microsoft.VisualStudio.Text.Classification;
using Microsoft.VisualStudio.Utilities;

namespace HlslLsp.VisualStudio;

internal static class HlslClassificationNames
{
    internal const string Keyword = "HLSL-LSP keyword";
    internal const string Preprocessor = "HLSL-LSP preprocessor";

#pragma warning disable CS0649
    [Export(typeof(ClassificationTypeDefinition))]
    [Name(Keyword)]
    [BaseDefinition(PredefinedClassificationTypeNames.Keyword)]
    internal static ClassificationTypeDefinition KeywordDefinition;

    [Export(typeof(ClassificationTypeDefinition))]
    [Name(Preprocessor)]
    [BaseDefinition(PredefinedClassificationTypeNames.Keyword)]
    internal static ClassificationTypeDefinition PreprocessorDefinition;
#pragma warning restore CS0649
}

[Export(typeof(IClassifierProvider))]
[ContentType(HlslContentDefinition.ContentTypeName)]
internal sealed class HlslClassifierProvider : IClassifierProvider
{
    [Import]
    internal IClassificationTypeRegistryService ClassificationRegistry { get; set; }

    public IClassifier GetClassifier(ITextBuffer textBuffer)
    {
        if (textBuffer == null)
        {
            throw new ArgumentNullException(nameof(textBuffer));
        }

        return textBuffer.Properties.GetOrCreateSingletonProperty(
            () => new HlslClassifier(textBuffer, ClassificationRegistry));
    }
}

internal sealed class HlslClassifier : IClassifier
{
    private static readonly HashSet<string> Keywords = new HashSet<string>(
        new[]
        {
            "break", "case", "cbuffer", "class", "const", "continue", "default", "discard",
            "do", "else", "enum", "extern", "false", "for", "groupshared", "if", "in",
            "inline", "inout", "namespace", "nointerpolation", "out", "precise", "register",
            "return", "row_major", "static", "struct", "switch", "template", "true",
            "typedef", "typename", "uniform", "using", "volatile", "while",
            "bool", "bool2", "bool3", "bool4", "double", "double2", "double3", "double4",
            "float", "float2", "float3", "float4", "float2x2", "float3x3", "float4x4",
            "half", "half2", "half3", "half4", "int", "int2", "int3", "int4",
            "uint", "uint2", "uint3", "uint4", "uint16_t", "uint32_t", "uint64_t",
            "int16_t", "int32_t", "int64_t", "void", "vector", "matrix",
            "Buffer", "ByteAddressBuffer", "ConstantBuffer", "RWBuffer",
            "RWByteAddressBuffer", "RWStructuredBuffer", "RWTexture1D", "RWTexture2D",
            "RWTexture3D", "SamplerComparisonState", "SamplerState", "StructuredBuffer",
            "Texture1D", "Texture1DArray", "Texture2D", "Texture2DArray", "Texture3D",
            "TextureCube", "TextureCubeArray",
        },
        StringComparer.Ordinal);

    private readonly IClassificationType keyword;
    private readonly IClassificationType preprocessor;
    private readonly IClassificationType comment;
    private readonly IClassificationType text;
    private readonly IClassificationType number;
    private readonly object cacheLock = new object();
    private ITextSnapshot cachedSnapshot;
    private IReadOnlyList<TokenSpan> cachedTokens = Array.Empty<TokenSpan>();

    internal HlslClassifier(ITextBuffer textBuffer, IClassificationTypeRegistryService registry)
    {
        keyword = registry.GetClassificationType(HlslClassificationNames.Keyword);
        preprocessor = registry.GetClassificationType(HlslClassificationNames.Preprocessor);
        comment = registry.GetClassificationType(PredefinedClassificationTypeNames.Comment);
        text = registry.GetClassificationType(PredefinedClassificationTypeNames.String);
        number = registry.GetClassificationType(PredefinedClassificationTypeNames.Number);
        textBuffer.Changed += OnBufferChanged;
    }

    public event EventHandler<ClassificationChangedEventArgs> ClassificationChanged;

    public IList<ClassificationSpan> GetClassificationSpans(SnapshotSpan span)
    {
        var tokens = TokensFor(span.Snapshot);
        var result = new List<ClassificationSpan>();
        foreach (var token in tokens)
        {
            if (token.End <= span.Start.Position || token.Start >= span.End.Position)
            {
                continue;
            }

            result.Add(new ClassificationSpan(
                new SnapshotSpan(span.Snapshot, token.Start, token.Length),
                token.Classification));
        }

        return result;
    }

    private IReadOnlyList<TokenSpan> TokensFor(ITextSnapshot snapshot)
    {
        lock (cacheLock)
        {
            if (!ReferenceEquals(snapshot, cachedSnapshot))
            {
                cachedTokens = Tokenize(snapshot.GetText());
                cachedSnapshot = snapshot;
            }

            return cachedTokens;
        }
    }

    private void OnBufferChanged(object sender, TextContentChangedEventArgs eventArgs)
    {
        lock (cacheLock)
        {
            cachedSnapshot = null;
            cachedTokens = Array.Empty<TokenSpan>();
        }

        ClassificationChanged?.Invoke(
            this,
            new ClassificationChangedEventArgs(
                new SnapshotSpan(eventArgs.After, 0, eventArgs.After.Length)));
    }

    private IReadOnlyList<TokenSpan> Tokenize(string source)
    {
        var result = new List<TokenSpan>();
        var offset = 0;
        var firstNonWhitespaceOnLine = true;
        while (offset < source.Length)
        {
            var current = source[offset];
            if (current == '\r' || current == '\n')
            {
                firstNonWhitespaceOnLine = true;
                offset++;
                continue;
            }
            if (char.IsWhiteSpace(current))
            {
                offset++;
                continue;
            }

            if (current == '/' && offset + 1 < source.Length && source[offset + 1] == '/')
            {
                var end = source.IndexOfAny(new[] { '\r', '\n' }, offset + 2);
                end = end < 0 ? source.Length : end;
                result.Add(new TokenSpan(offset, end - offset, comment));
                offset = end;
                firstNonWhitespaceOnLine = false;
                continue;
            }
            if (current == '/' && offset + 1 < source.Length && source[offset + 1] == '*')
            {
                var end = source.IndexOf("*/", offset + 2, StringComparison.Ordinal);
                end = end < 0 ? source.Length : end + 2;
                result.Add(new TokenSpan(offset, end - offset, comment));
                offset = end;
                firstNonWhitespaceOnLine = false;
                continue;
            }
            if (current == '"' || current == '\'')
            {
                var quote = current;
                var end = offset + 1;
                while (end < source.Length)
                {
                    if (source[end] == '\\' && end + 1 < source.Length)
                    {
                        end += 2;
                    }
                    else if (source[end++] == quote)
                    {
                        break;
                    }
                }
                result.Add(new TokenSpan(offset, end - offset, text));
                offset = end;
                firstNonWhitespaceOnLine = false;
                continue;
            }
            if (current == '#' && firstNonWhitespaceOnLine)
            {
                var end = offset + 1;
                while (end < source.Length && char.IsWhiteSpace(source[end]) &&
                       source[end] != '\r' && source[end] != '\n')
                {
                    end++;
                }
                while (end < source.Length && IsIdentifierPart(source[end]))
                {
                    end++;
                }
                result.Add(new TokenSpan(offset, end - offset, preprocessor));
                offset = end;
                firstNonWhitespaceOnLine = false;
                continue;
            }
            if (IsIdentifierStart(current))
            {
                var end = offset + 1;
                while (end < source.Length && IsIdentifierPart(source[end]))
                {
                    end++;
                }
                if (Keywords.Contains(source.Substring(offset, end - offset)))
                {
                    result.Add(new TokenSpan(offset, end - offset, keyword));
                }
                offset = end;
                firstNonWhitespaceOnLine = false;
                continue;
            }
            if (char.IsDigit(current) ||
                (current == '.' && offset + 1 < source.Length && char.IsDigit(source[offset + 1])))
            {
                var end = offset + 1;
                while (end < source.Length &&
                       (char.IsLetterOrDigit(source[end]) || source[end] == '.' ||
                        source[end] == '_'))
                {
                    end++;
                }
                result.Add(new TokenSpan(offset, end - offset, number));
                offset = end;
                firstNonWhitespaceOnLine = false;
                continue;
            }

            firstNonWhitespaceOnLine = false;
            offset++;
        }

        return result;
    }

    private static bool IsIdentifierStart(char value) =>
        value == '_' || char.IsLetter(value);

    private static bool IsIdentifierPart(char value) =>
        value == '_' || char.IsLetterOrDigit(value);

    private readonly struct TokenSpan
    {
        internal TokenSpan(int start, int length, IClassificationType classification)
        {
            Start = start;
            Length = length;
            Classification = classification;
        }

        internal int Start { get; }
        internal int Length { get; }
        internal int End => Start + Length;
        internal IClassificationType Classification { get; }
    }
}
