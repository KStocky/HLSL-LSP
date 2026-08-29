using System;
using System.ComponentModel.Composition;
using System.Threading;
using System.Threading.Tasks;
using HlslLsp.VisualStudio.Bootstrap;
using Microsoft.VisualStudio.Language.Intellisense;
using Microsoft.VisualStudio.Language.StandardClassification;
using Microsoft.VisualStudio.Text;
using Microsoft.VisualStudio.Text.Adornments;
using Microsoft.VisualStudio.Utilities;

namespace HlslLsp.VisualStudio;

[Export(typeof(IAsyncQuickInfoSourceProvider))]
[Name("HLSL-LSP memory layout action")]
[ContentType("HLSL")]
[ContentType("HLSLHeader")]
[ContentType("HLSL-LSP-Colored")]
[ContentType("HLSLHeader-LSP-Colored")]
internal sealed class MemoryLayoutQuickInfoSourceProvider : IAsyncQuickInfoSourceProvider
{
    [Import]
    internal ITextDocumentFactoryService TextDocuments { get; set; }

    public IAsyncQuickInfoSource TryCreateQuickInfoSource(ITextBuffer textBuffer)
        => new MemoryLayoutQuickInfoSource(textBuffer, TextDocuments);
}

internal sealed class MemoryLayoutQuickInfoSource : IAsyncQuickInfoSource
{
    private readonly ITextBuffer buffer;
    private readonly ITextDocumentFactoryService textDocuments;

    internal MemoryLayoutQuickInfoSource(
        ITextBuffer buffer,
        ITextDocumentFactoryService textDocuments)
    {
        this.buffer = buffer;
        this.textDocuments = textDocuments;
    }

    public async Task<QuickInfoItem> GetQuickInfoItemAsync(
        IAsyncQuickInfoSession session,
        CancellationToken cancellationToken)
    {
        var point = session.GetTriggerPoint(buffer.CurrentSnapshot);
        if (point == null ||
            !textDocuments.TryGetTextDocument(buffer, out var document))
        {
            return null;
        }
        var line = point.Value.GetContainingLine();
        var lineNumber = line.LineNumber;
        var character = point.Value.Position - line.Start.Position;
        var uri = new Uri(document.FilePath);
        var layout = await MemoryLayoutBridge.RequestAsync(
                uri,
                lineNumber,
                character,
                cancellationToken)
            .ConfigureAwait(false);
        if (layout == null)
        {
            return null;
        }

        var trackingSpan = buffer.CurrentSnapshot.CreateTrackingSpan(
            point.Value.Position,
            0,
            SpanTrackingMode.EdgeInclusive);
        var action = new ClassifiedTextElement(
            new ClassifiedTextRun(
                PredefinedClassificationTypeNames.Identifier,
                "Memory Layout",
                new Action(() => MemoryLayoutBridge.Show(uri, lineNumber, character)),
                "Open the HLSL memory layout view"));
        return new QuickInfoItem(
            trackingSpan,
            new ContainerElement(ContainerElementStyle.Wrapped, action));
    }

    public void Dispose()
    {
    }
}
