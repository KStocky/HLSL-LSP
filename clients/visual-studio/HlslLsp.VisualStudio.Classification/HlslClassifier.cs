using System;
using System.Collections.Generic;
using System.ComponentModel.Composition;
using System.Diagnostics;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.VisualStudio.Language.StandardClassification;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Text;
using Microsoft.VisualStudio.Text.Classification;
using Microsoft.VisualStudio.Utilities;

namespace HlslLsp.VisualStudio;

internal static class HlslClassificationNames
{
    internal const string Keyword = "HLSL-LSP keyword";
    internal const string Preprocessor = "HLSL-LSP preprocessor";
    internal const string Macro = "HLSL-LSP macro";
    internal const string Function = "HLSL-LSP function";
    internal const string Type = "HLSL-LSP type";
    internal const string Comment = "HLSL-LSP comment";
    internal const string String = "HLSL-LSP string";
    internal const string Number = "HLSL-LSP number";

#pragma warning disable CS0649
    [Export(typeof(ClassificationTypeDefinition))]
    [Name(Keyword)]
    [BaseDefinition(PredefinedClassificationTypeNames.Keyword)]
    internal static ClassificationTypeDefinition KeywordDefinition;

    [Export(typeof(ClassificationTypeDefinition))]
    [Name(Preprocessor)]
    [BaseDefinition(PredefinedClassificationTypeNames.Keyword)]
    internal static ClassificationTypeDefinition PreprocessorDefinition;

    [Export(typeof(ClassificationTypeDefinition))]
    [Name(Macro)]
    [BaseDefinition(PredefinedClassificationTypeNames.FormalLanguage)]
    internal static ClassificationTypeDefinition MacroDefinition;

    [Export(typeof(ClassificationTypeDefinition))]
    [Name(Function)]
    [BaseDefinition(PredefinedClassificationTypeNames.Method)]
    internal static ClassificationTypeDefinition FunctionDefinition;

    [Export(typeof(ClassificationTypeDefinition))]
    [Name(Type)]
    [BaseDefinition(PredefinedClassificationTypeNames.Type)]
    internal static ClassificationTypeDefinition TypeDefinition;

    [Export(typeof(ClassificationTypeDefinition))]
    [Name(Comment)]
    [BaseDefinition(PredefinedClassificationTypeNames.Comment)]
    internal static ClassificationTypeDefinition CommentDefinition;

    [Export(typeof(ClassificationTypeDefinition))]
    [Name(String)]
    [BaseDefinition(PredefinedClassificationTypeNames.String)]
    internal static ClassificationTypeDefinition StringDefinition;

    [Export(typeof(ClassificationTypeDefinition))]
    [Name(Number)]
    [BaseDefinition(PredefinedClassificationTypeNames.Number)]
    internal static ClassificationTypeDefinition NumberDefinition;
#pragma warning restore CS0649
}

[Export(typeof(IClassifierProvider))]
[ContentType("text")]
[ContentType("HLSL")]
[ContentType("HLSLHeader")]
[ContentType("HLSL-LSP-Colored")]
[ContentType("HLSLHeader-LSP-Colored")]
internal sealed class HlslClassifierProvider : IClassifierProvider
{
    [Import]
    internal IClassificationTypeRegistryService ClassificationRegistry { get; set; }

    [Import]
    internal ITextDocumentFactoryService TextDocuments { get; set; }

    public IClassifier GetClassifier(ITextBuffer textBuffer)
    {
        if (textBuffer == null)
        {
            throw new ArgumentNullException(nameof(textBuffer));
        }

        if (!TextDocuments.TryGetTextDocument(textBuffer, out var document))
        {
            return null;
        }

        var extension = Path.GetExtension(document.FilePath);
        if (!extension.Equals(".hlsl", StringComparison.OrdinalIgnoreCase) &&
            !extension.Equals(".hlsli", StringComparison.OrdinalIgnoreCase) &&
            !extension.Equals(".usf", StringComparison.OrdinalIgnoreCase))
        {
            return null;
        }

        return textBuffer.Properties.GetOrCreateSingletonProperty(
            () => new HlslClassifier(textBuffer, ClassificationRegistry));
    }
}

internal sealed class HlslClassifier : IClassifier
{
    private static readonly HashSet<string> Keywords = new(
        new[]
        {
            "break", "case", "cbuffer", "class", "const", "continue", "default",
            "discard", "do", "else", "enum", "extern", "false", "for",
            "groupshared", "if", "in", "inline", "inout", "namespace",
            "nointerpolation", "out", "precise", "register", "return", "row_major",
            "static", "struct", "switch", "template", "true", "typedef", "typename",
            "uniform", "using", "volatile", "while",
        },
        StringComparer.Ordinal);

    private static readonly HashSet<string> BuiltInTypes = new(
        new[]
        {
            "bool", "bool2", "bool3", "bool4", "double", "double2", "double3",
            "double4", "float", "float2", "float3", "float4", "float2x2",
            "float3x3", "float4x4", "half", "half2", "half3", "half4", "int",
            "int2", "int3", "int4", "uint", "uint2", "uint3", "uint4",
            "uint16_t", "uint32_t", "uint64_t", "int16_t", "int32_t", "int64_t",
            "void", "vector", "matrix", "Buffer", "ByteAddressBuffer",
            "ConstantBuffer", "RWBuffer", "RWByteAddressBuffer",
            "RWStructuredBuffer", "RWTexture1D", "RWTexture2D", "RWTexture3D",
            "SamplerComparisonState", "SamplerState", "StructuredBuffer",
            "Texture1D", "Texture1DArray", "Texture2D", "Texture2DArray",
            "Texture3D", "TextureCube", "TextureCubeArray",
        },
        StringComparer.Ordinal);

    private readonly IClassificationType keyword;
    private readonly IClassificationType preprocessor;
    private readonly IClassificationType macro;
    private readonly IClassificationType function;
    private readonly IClassificationType type;
    private readonly IClassificationType comment;
    private readonly IClassificationType text;
    private readonly IClassificationType number;
    private readonly IClassificationType excludedCode;
    private readonly ITextBuffer textBuffer;
    private readonly object scheduleLock = new();
    private ClassificationCache cache;
    private ITextSnapshot scheduledSnapshot;
    private CancellationTokenSource tokenizationCancellation;
    private Task tokenizationTask;

    internal HlslClassifier(
        ITextBuffer textBuffer,
        IClassificationTypeRegistryService registry)
    {
        this.textBuffer = textBuffer ?? throw new ArgumentNullException(nameof(textBuffer));
        keyword = registry.GetClassificationType(HlslClassificationNames.Keyword);
        preprocessor = registry.GetClassificationType(HlslClassificationNames.Preprocessor);
        macro = registry.GetClassificationType(HlslClassificationNames.Macro);
        function = registry.GetClassificationType(HlslClassificationNames.Function);
        type = registry.GetClassificationType(HlslClassificationNames.Type);
        comment = registry.GetClassificationType(HlslClassificationNames.Comment);
        text = registry.GetClassificationType(HlslClassificationNames.String);
        number = registry.GetClassificationType(HlslClassificationNames.Number);
        excludedCode = registry.GetClassificationType(
            PredefinedClassificationTypeNames.ExcludedCode);
        textBuffer.Changed += OnTextBufferChanged;
        ScheduleTokenization(textBuffer.CurrentSnapshot);
    }

    public event EventHandler<ClassificationChangedEventArgs> ClassificationChanged;

    public IList<ClassificationSpan> GetClassificationSpans(SnapshotSpan span)
    {
        var currentCache = Volatile.Read(ref cache);
        if (span.IsEmpty ||
            currentCache == null ||
            !ReferenceEquals(currentCache.Snapshot, span.Snapshot))
        {
            ScheduleTokenization(span.Snapshot);
            return Array.Empty<ClassificationSpan>();
        }

        var result = new List<ClassificationSpan>();
        var tokens = currentCache.Tokens;
        for (var index = FirstTokenEndingAfter(tokens, span.Start.Position);
             index < tokens.Count;
             index++)
        {
            var token = tokens[index];
            if (token.Start >= span.End.Position)
            {
                break;
            }

            result.Add(
                new ClassificationSpan(
                    new SnapshotSpan(span.Snapshot, token.Start, token.Length),
                    token.Classification));
        }

        return result;
    }

    private void OnTextBufferChanged(object sender, TextContentChangedEventArgs eventArgs)
    {
        ScheduleTokenization(eventArgs.After);
    }

    private void ScheduleTokenization(ITextSnapshot snapshot)
    {
        CancellationToken cancellationToken;
        lock (scheduleLock)
        {
            var currentCache = Volatile.Read(ref cache);
            if ((currentCache != null && ReferenceEquals(currentCache.Snapshot, snapshot)) ||
                ReferenceEquals(scheduledSnapshot, snapshot))
            {
                return;
            }

            if (tokenizationTask != null && !tokenizationTask.IsCompleted)
            {
                tokenizationCancellation?.Cancel();
            }
            tokenizationCancellation?.Dispose();
            tokenizationCancellation = new CancellationTokenSource();
            cancellationToken = tokenizationCancellation.Token;
            scheduledSnapshot = snapshot;
        }

        tokenizationTask = Task.Run(
            async () =>
            {
                try
                {
                    var tokens = Tokenize(snapshot.GetText());
                    cancellationToken.ThrowIfCancellationRequested();
                    await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync(
                        cancellationToken);
                    if (cancellationToken.IsCancellationRequested ||
                        !ReferenceEquals(textBuffer.CurrentSnapshot, snapshot))
                    {
                        return;
                    }

                    Volatile.Write(ref cache, new ClassificationCache(snapshot, tokens));
                    lock (scheduleLock)
                    {
                        if (ReferenceEquals(scheduledSnapshot, snapshot))
                        {
                            scheduledSnapshot = null;
                        }
                    }

                    ClassificationChanged?.Invoke(
                        this,
                        new ClassificationChangedEventArgs(
                            new SnapshotSpan(snapshot, 0, snapshot.Length)));
                }
                catch (OperationCanceledException)
                {
                }
                catch (Exception error)
                {
                    Debug.WriteLine(
                        $"HLSL-LSP: Background classification failed: {error}");
                }
            },
            cancellationToken);
    }

    private IReadOnlyList<TokenSpan> Tokenize(string source)
    {
        var result = new List<TokenSpan>();
        var excludedRanges = FindExcludedRanges(source);
        var excludedRangeIndex = 0;
        var userTypes = new HashSet<string>(StringComparer.Ordinal);
        var offset = 0;
        var firstNonWhitespaceOnLine = true;
        var expectTypeName = false;
        while (offset < source.Length)
        {
            if (excludedRangeIndex < excludedRanges.Count &&
                offset == excludedRanges[excludedRangeIndex].Start)
            {
                var range = excludedRanges[excludedRangeIndex++];
                result.Add(new TokenSpan(range.Start, range.Length, excludedCode));
                offset = range.End;
                firstNonWhitespaceOnLine = false;
                continue;
            }

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

            if (current == '/' &&
                offset + 1 < source.Length &&
                source[offset + 1] == '/')
            {
                var end = source.IndexOfAny(new[] { '\r', '\n' }, offset + 2);
                end = end < 0 ? source.Length : end;
                result.Add(new TokenSpan(offset, end - offset, comment));
                offset = end;
                firstNonWhitespaceOnLine = false;
                continue;
            }
            if (current == '/' &&
                offset + 1 < source.Length &&
                source[offset + 1] == '*')
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
                while (end < source.Length &&
                       char.IsWhiteSpace(source[end]) &&
                       source[end] != '\r' &&
                       source[end] != '\n')
                {
                    end++;
                }
                var directiveStart = end;
                while (end < source.Length && IsIdentifierPart(source[end]))
                {
                    end++;
                }
                result.Add(new TokenSpan(offset, end - offset, preprocessor));
                var directive = source.Substring(directiveStart, end - directiveStart);
                if (directive == "define" ||
                    directive == "ifdef" ||
                    directive == "ifndef" ||
                    directive == "undef")
                {
                    var macroStart = end;
                    while (macroStart < source.Length &&
                           (source[macroStart] == ' ' || source[macroStart] == '\t'))
                    {
                        macroStart++;
                    }
                    var macroEnd = macroStart;
                    while (macroEnd < source.Length && IsIdentifierPart(source[macroEnd]))
                    {
                        macroEnd++;
                    }
                    if (macroEnd > macroStart)
                    {
                        result.Add(
                            new TokenSpan(macroStart, macroEnd - macroStart, macro));
                        end = macroEnd;
                    }
                }
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

                var identifier = source.Substring(offset, end - offset);
                if (expectTypeName && Keywords.Contains(identifier))
                {
                    result.Add(new TokenSpan(offset, end - offset, keyword));
                }
                else if (expectTypeName)
                {
                    userTypes.Add(identifier);
                    result.Add(new TokenSpan(offset, end - offset, type));
                    expectTypeName = false;
                }
                else if (BuiltInTypes.Contains(identifier) || userTypes.Contains(identifier))
                {
                    result.Add(new TokenSpan(offset, end - offset, type));
                }
                else if (Keywords.Contains(identifier))
                {
                    result.Add(new TokenSpan(offset, end - offset, keyword));
                    expectTypeName =
                        identifier == "struct" ||
                        identifier == "class" ||
                        identifier == "enum" ||
                        identifier == "typename";
                }
                else
                {
                    var next = end;
                    while (next < source.Length && char.IsWhiteSpace(source[next]))
                    {
                        next++;
                    }
                    if (next < source.Length && source[next] == '(')
                    {
                        result.Add(new TokenSpan(offset, end - offset, function));
                    }
                }

                offset = end;
                firstNonWhitespaceOnLine = false;
                continue;
            }
            if (char.IsDigit(current) ||
                (current == '.' &&
                 offset + 1 < source.Length &&
                 char.IsDigit(source[offset + 1])))
            {
                var end = offset + 1;
                while (end < source.Length &&
                       (char.IsLetterOrDigit(source[end]) ||
                        source[end] == '.' ||
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

    private static IReadOnlyList<TextRange> FindExcludedRanges(string source)
    {
        var ranges = new List<TextRange>();
        var conditionals = new Stack<ConditionalState>();
        var inBlockComment = false;
        var lineStart = 0;
        while (lineStart < source.Length)
        {
            var lineEnd = source.IndexOfAny(new[] { '\r', '\n' }, lineStart);
            lineEnd = lineEnd < 0 ? source.Length : lineEnd;
            var directiveStart = FindDirectiveStart(
                source,
                lineStart,
                lineEnd,
                ref inBlockComment);
            var isDirective = directiveStart >= 0;
            if (!isDirective && !CurrentBranchIsActive(conditionals) && lineEnd > lineStart)
            {
                ranges.Add(new TextRange(lineStart, lineEnd - lineStart));
            }
            else if (isDirective)
            {
                UpdateConditionalState(
                    source.Substring(directiveStart + 1, lineEnd - directiveStart - 1),
                    conditionals);
            }

            lineStart = lineEnd;
            if (lineStart < source.Length && source[lineStart] == '\r')
            {
                lineStart++;
            }
            if (lineStart < source.Length && source[lineStart] == '\n')
            {
                lineStart++;
            }
        }
        return ranges;
    }

    private static int FindDirectiveStart(
        string source,
        int lineStart,
        int lineEnd,
        ref bool inBlockComment)
    {
        var directiveStart = -1;
        var sawNonTrivia = false;
        var quote = '\0';
        for (var index = lineStart; index < lineEnd; index++)
        {
            if (inBlockComment)
            {
                if (source[index] == '*' &&
                    index + 1 < lineEnd &&
                    source[index + 1] == '/')
                {
                    inBlockComment = false;
                    index++;
                }
                continue;
            }
            if (quote != '\0')
            {
                if (source[index] == '\\' && index + 1 < lineEnd)
                {
                    index++;
                }
                else if (source[index] == quote)
                {
                    quote = '\0';
                }
                continue;
            }
            if (source[index] == '/' && index + 1 < lineEnd)
            {
                if (source[index + 1] == '/')
                {
                    break;
                }
                if (source[index + 1] == '*')
                {
                    inBlockComment = true;
                    index++;
                    continue;
                }
            }
            if (source[index] == '"' || source[index] == '\'')
            {
                quote = source[index];
                sawNonTrivia = true;
            }
            else if (!char.IsWhiteSpace(source[index]))
            {
                if (!sawNonTrivia && source[index] == '#')
                {
                    directiveStart = index;
                }
                sawNonTrivia = true;
            }
        }
        return directiveStart;
    }

    private static bool CurrentBranchIsActive(Stack<ConditionalState> conditionals) =>
        conditionals.Count == 0 || conditionals.Peek().CurrentBranchIsActive;

    private static void UpdateConditionalState(
        string directiveText,
        Stack<ConditionalState> conditionals)
    {
        var index = 0;
        SkipWhitespace(directiveText, ref index);
        var nameStart = index;
        while (index < directiveText.Length && IsIdentifierPart(directiveText[index]))
        {
            index++;
        }
        var name = directiveText.Substring(nameStart, index - nameStart);
        SkipWhitespace(directiveText, ref index);
        var expression = directiveText.Substring(index).Trim();

        if (name == "if")
        {
            var parentIsActive = CurrentBranchIsActive(conditionals);
            var condition = !IsLiteralFalse(expression);
            conditionals.Push(
                new ConditionalState(parentIsActive, condition, parentIsActive && condition));
        }
        else if (name == "ifdef" || name == "ifndef")
        {
            var parentIsActive = CurrentBranchIsActive(conditionals);
            conditionals.Push(new ConditionalState(parentIsActive, true, parentIsActive));
        }
        else if (name == "else" && conditionals.Count != 0)
        {
            var state = conditionals.Pop();
            conditionals.Push(
                new ConditionalState(
                    state.ParentIsActive,
                    true,
                    state.ParentIsActive && !state.BranchTaken));
        }
        else if (name == "elif" && conditionals.Count != 0)
        {
            var state = conditionals.Pop();
            var condition = !IsLiteralFalse(expression);
            conditionals.Push(
                new ConditionalState(
                    state.ParentIsActive,
                    state.BranchTaken || condition,
                    state.ParentIsActive && !state.BranchTaken && condition));
        }
        else if (name == "endif" && conditionals.Count != 0)
        {
            conditionals.Pop();
        }
    }

    private static bool IsLiteralFalse(string expression)
    {
        var commentStart = expression.IndexOf("//", StringComparison.Ordinal);
        if (commentStart >= 0)
        {
            expression = expression.Substring(0, commentStart).TrimEnd();
        }
        return expression == "0" ||
               expression.Equals("false", StringComparison.OrdinalIgnoreCase);
    }

    private static void SkipWhitespace(string value, ref int index)
    {
        while (index < value.Length && char.IsWhiteSpace(value[index]))
        {
            index++;
        }
    }

    private static int FirstTokenEndingAfter(
        IReadOnlyList<TokenSpan> tokens,
        int position)
    {
        var lower = 0;
        var upper = tokens.Count;
        while (lower < upper)
        {
            var middle = lower + ((upper - lower) / 2);
            if (tokens[middle].End <= position)
            {
                lower = middle + 1;
            }
            else
            {
                upper = middle;
            }
        }
        return lower;
    }

    private static bool IsIdentifierStart(char value) =>
        value == '_' || char.IsLetter(value);

    private static bool IsIdentifierPart(char value) =>
        value == '_' || char.IsLetterOrDigit(value);

    private readonly struct TokenSpan
    {
        internal TokenSpan(
            int start,
            int length,
            IClassificationType classification)
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

    private readonly struct TextRange
    {
        internal TextRange(int start, int length)
        {
            Start = start;
            Length = length;
        }

        internal int Start { get; }
        internal int Length { get; }
        internal int End => Start + Length;
    }

    private readonly struct ConditionalState
    {
        internal ConditionalState(
            bool parentIsActive,
            bool branchTaken,
            bool currentBranchIsActive)
        {
            ParentIsActive = parentIsActive;
            BranchTaken = branchTaken;
            CurrentBranchIsActive = currentBranchIsActive;
        }

        internal bool ParentIsActive { get; }
        internal bool BranchTaken { get; }
        internal bool CurrentBranchIsActive { get; }
    }

    private sealed class ClassificationCache
    {
        internal ClassificationCache(
            ITextSnapshot snapshot,
            IReadOnlyList<TokenSpan> tokens)
        {
            Snapshot = snapshot;
            Tokens = tokens;
        }

        internal ITextSnapshot Snapshot { get; }
        internal IReadOnlyList<TokenSpan> Tokens { get; }
    }
}
