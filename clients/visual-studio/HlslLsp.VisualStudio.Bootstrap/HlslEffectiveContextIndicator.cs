using System;
using System.Threading;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Text;
using Microsoft.VisualStudio.Text.Editor;

namespace HlslLsp.VisualStudio.Bootstrap;

internal static class HlslEffectiveContextAdornmentLayer
{
    internal const string LayerName = "HlslEffectiveContextIndicator";
}

internal static class HlslEffectiveContextIndicatorDisplay
{
    internal static string Compact(EffectiveShaderContextModel context)
        => context == null
            ? string.Empty
            : $"HLSL · {EffectiveShaderContextDisplay.Variant(context.ActiveVariant)} · " +
              $"{EffectiveShaderContextDisplay.Value(context.EntryPoint)} · " +
              $"{EffectiveShaderContextDisplay.Value(context.TargetProfile)}";

    internal static string Tooltip(EffectiveShaderContextModel context)
    {
        var summary = EffectiveShaderContextDisplay.Summary(context);
        var origins = EffectiveShaderContextDisplay.OriginSummary(context);
        return string.IsNullOrEmpty(origins)
            ? summary + Environment.NewLine + "Click to select a shader variant."
            : summary + Environment.NewLine + "Configuration origins: " + origins +
              Environment.NewLine + "Click to select a shader variant.";
    }
}

internal sealed class HlslEffectiveContextDocumentState : IDisposable
{
    private bool disposed;

    internal HlslEffectiveContextDocumentState(string filePath)
    {
        CurrentUri = FileUri(filePath);
    }

    internal Uri CurrentUri { get; private set; }

    internal bool Apply(TextDocumentFileActionEventArgs eventArgs)
    {
        if (disposed ||
            (eventArgs.FileActionType & FileActionTypes.DocumentRenamed) == 0)
        {
            return false;
        }

        var next = FileUri(eventArgs.FilePath);
        var changed = !Equals(CurrentUri, next);
        CurrentUri = next;
        return changed;
    }

    public void Dispose()
    {
        disposed = true;
        CurrentUri = null;
    }

    private static Uri FileUri(string filePath)
    {
        if (string.IsNullOrWhiteSpace(filePath))
        {
            return null;
        }

        try
        {
            return new Uri(System.IO.Path.GetFullPath(filePath));
        }
        catch (Exception error) when (
            error is ArgumentException ||
            error is NotSupportedException ||
            error is System.IO.PathTooLongException ||
            error is System.Security.SecurityException ||
            error is UriFormatException)
        {
            return null;
        }
    }
}

internal sealed class HlslEffectiveContextIndicator : IDisposable
{
    private static readonly TimeSpan RefreshDelay = TimeSpan.FromMilliseconds(125);
    private static event Action Invalidated;

    private readonly IWpfTextView view;
    private readonly ITextDocument document;
    private readonly HlslEffectiveContextDocumentState documentState;
    private readonly IAdornmentLayer layer;
    private readonly Border indicator;
    private readonly TextBlock label;
    private CancellationTokenSource refreshCancellation;
    private int generation;
    private bool disposed;

    internal HlslEffectiveContextIndicator(IWpfTextView view, ITextDocument document)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        this.view = view;
        this.document = document ?? throw new ArgumentNullException(nameof(document));
        documentState = new HlslEffectiveContextDocumentState(document.FilePath);
        layer = view.GetAdornmentLayer(HlslEffectiveContextAdornmentLayer.LayerName);
        label = new TextBlock
        {
            FontSize = 11,
            Foreground = SystemColors.HighlightTextBrush,
            TextTrimming = TextTrimming.CharacterEllipsis,
            MaxWidth = 420,
        };
        indicator = new Border
        {
            Background = new SolidColorBrush(Color.FromArgb(224, 45, 45, 48)),
            BorderBrush = new SolidColorBrush(Color.FromArgb(180, 104, 104, 104)),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(3),
            Padding = new Thickness(6, 2, 6, 2),
            Child = label,
            Cursor = Cursors.Hand,
            Visibility = Visibility.Collapsed,
        };
        indicator.MouseLeftButtonUp += OnClick;
        view.LayoutChanged += OnLayoutChanged;
        view.TextBuffer.Changed += OnBufferChanged;
        view.TextBuffer.ContentTypeChanged += OnContentTypeChanged;
        view.Closed += OnClosed;
        document.FileActionOccurred += OnFileActionOccurred;
        Invalidated += OnInvalidated;
        layer.AddAdornment(
            AdornmentPositioningBehavior.ViewportRelative,
            null,
            this,
            indicator,
            null);
        ScheduleRefresh();
    }

    internal static void InvalidateAll() => Invalidated?.Invoke();

    public void Dispose()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (disposed)
        {
            return;
        }
        disposed = true;
        ++generation;
        refreshCancellation?.Cancel();
        refreshCancellation?.Dispose();
        view.LayoutChanged -= OnLayoutChanged;
        view.TextBuffer.Changed -= OnBufferChanged;
        view.TextBuffer.ContentTypeChanged -= OnContentTypeChanged;
        view.Closed -= OnClosed;
        document.FileActionOccurred -= OnFileActionOccurred;
        Invalidated -= OnInvalidated;
        indicator.MouseLeftButtonUp -= OnClick;
        documentState.Dispose();
        layer.RemoveAdornmentsByTag(this);
    }

    private static bool IsHlsl(ITextBuffer buffer)
        => buffer.ContentType.IsOfType("HLSL") ||
           buffer.ContentType.IsOfType("HLSLHeader");

    private void ScheduleRefresh()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        refreshCancellation?.Cancel();
        refreshCancellation?.Dispose();
        refreshCancellation = null;
        indicator.Visibility = Visibility.Collapsed;
        if (disposed || !IsHlsl(view.TextBuffer))
        {
            return;
        }
        var documentUri = documentState.CurrentUri;
        if (documentUri == null)
        {
            return;
        }
        var cancellation = new CancellationTokenSource();
        refreshCancellation = cancellation;
        var requestGeneration = ++generation;
        HlslBootstrapPackage.RunInBackground(
            () => RefreshAsync(documentUri, cancellation, requestGeneration),
            "HlslLsp/EffectiveContextIndicator");
    }

    private async Task RefreshAsync(
        Uri documentUri,
        CancellationTokenSource cancellation,
        int requestGeneration)
    {
        try
        {
            await Task.Delay(RefreshDelay, cancellation.Token).ConfigureAwait(false);
            var context = await EffectiveShaderContextBridge.RequestAsync(
                    documentUri,
                    cancellation.Token)
                .ConfigureAwait(false);
            if (context == null ||
                cancellation.IsCancellationRequested ||
                requestGeneration != Volatile.Read(ref generation))
            {
                return;
            }
            await ThreadHelper.JoinableTaskFactory.SwitchToMainThreadAsync(cancellation.Token);
            if (disposed ||
                requestGeneration != generation ||
                !IsHlsl(view.TextBuffer))
            {
                return;
            }
            label.Text = HlslEffectiveContextIndicatorDisplay.Compact(context);
            indicator.ToolTip = HlslEffectiveContextIndicatorDisplay.Tooltip(context);
            indicator.Visibility = Visibility.Visible;
            Position();
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception error)
        {
            ActivityLog.LogWarning(
                nameof(HlslEffectiveContextIndicator),
                "Could not refresh the effective shader context indicator: " + error.Message);
        }
    }

    private void Position()
    {
        indicator.Measure(new Size(double.PositiveInfinity, double.PositiveInfinity));
        Canvas.SetLeft(
            indicator,
            Math.Max(view.ViewportLeft + 8, view.ViewportRight - indicator.DesiredSize.Width - 12));
        Canvas.SetTop(indicator, view.ViewportTop + 8);
    }

    private void OnLayoutChanged(object sender, TextViewLayoutChangedEventArgs eventArgs)
    {
        _ = sender;
        _ = eventArgs;
        if (indicator.Visibility == Visibility.Visible)
        {
            Position();
        }
    }

    private void OnBufferChanged(object sender, TextContentChangedEventArgs eventArgs)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        _ = sender;
        _ = eventArgs;
        ScheduleRefresh();
    }

    private void OnContentTypeChanged(object sender, ContentTypeChangedEventArgs eventArgs)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        _ = sender;
        _ = eventArgs;
        ScheduleRefresh();
    }

    private void OnFileActionOccurred(
        object sender,
        TextDocumentFileActionEventArgs eventArgs)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        _ = sender;
        if ((eventArgs.FileActionType & FileActionTypes.DocumentRenamed) == 0)
        {
            return;
        }

        documentState.Apply(eventArgs);
        ScheduleRefresh();
    }

    private void OnInvalidated()
        => HlslBootstrapPackage.RunOnMainThread(ScheduleRefresh);

    private void OnClick(object sender, MouseButtonEventArgs eventArgs)
    {
        _ = sender;
        eventArgs.Handled = true;
        HlslBootstrapPackage.ExecuteSelectVariantCommand();
    }

    private void OnClosed(object sender, EventArgs eventArgs)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        _ = sender;
        _ = eventArgs;
        Dispose();
    }
}
