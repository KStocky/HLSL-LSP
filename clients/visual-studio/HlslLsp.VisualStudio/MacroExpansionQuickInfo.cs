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
[Name("HLSL-LSP macro expansion action")]
[ContentType("HLSL")]
[ContentType("HLSLHeader")]
internal sealed class MacroExpansionQuickInfoSourceProvider : IAsyncQuickInfoSourceProvider
{
    [Import]
    internal ITextDocumentFactoryService TextDocuments { get; set; }

    public IAsyncQuickInfoSource TryCreateQuickInfoSource(ITextBuffer textBuffer)
        => textBuffer.Properties.GetOrCreateSingletonProperty(
            typeof(MacroExpansionQuickInfoSource),
            () => new MacroExpansionQuickInfoSource(textBuffer, TextDocuments));
}

internal sealed class MacroExpansionQuickInfoSource : IAsyncQuickInfoSource
{
    private readonly ITextBuffer buffer;
    private readonly ITextDocumentFactoryService textDocuments;

    internal MacroExpansionQuickInfoSource(
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
        var context = await HlslCommandContextBridge.RequestAsync(
                uri,
                lineNumber,
                character,
                cancellationToken)
            .ConfigureAwait(false);
        if (context?.MacroExpansionAvailable != true)
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
                "Expand Macro",
                new Action(() => MacroExpansionBridge.Show(uri, lineNumber, character)),
                $"Show the compiler expansion of {context.MacroName}"));
        return new QuickInfoItem(
            trackingSpan,
            new ContainerElement(ContainerElementStyle.Wrapped, action));
    }

    public void Dispose()
    {
    }
}
