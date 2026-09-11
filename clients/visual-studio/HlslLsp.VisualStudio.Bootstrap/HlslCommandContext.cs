using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Text;
using Microsoft.VisualStudio.Text.Editor;
using Microsoft.VisualStudio.Threading;

namespace HlslLsp.VisualStudio.Bootstrap;

public sealed class HlslCommandContextModel
{
    public bool MemoryLayoutAvailable { get; set; }

    public string MemoryLayoutTarget { get; set; }

    public bool CallHierarchyAvailable { get; set; }

    public string CallableName { get; set; }

    public bool EntryPointDataFlowAvailable { get; set; }

    public bool ComputeVisualizationAvailable { get; set; }

    public string EntryPoint { get; set; }
}

internal enum HlslCommandKind
{
    MemoryLayout,
    SelectVariant,
    OpenEffectiveConfiguration,
    Compilation,
    ResourceBindings,
    PreprocessorExplorer,
    EntryPointDataFlow,
    CallHierarchy,
    ComputeVisualization,
}

internal static class HlslCommandIds
{
    internal const int MemoryLayout = 0x0100;
    internal const int SelectVariant = 0x0101;
    internal const int Compilation = 0x0102;
    internal const int ResourceBindings = 0x0103;
    internal const int PreprocessorExplorer = 0x0104;
    internal const int EntryPointDataFlow = 0x0105;
    internal const int CallHierarchy = 0x0106;
    internal const int ComputeVisualization = 0x0107;
    internal const int OpenEffectiveConfiguration = 0x0108;

    internal static HlslCommandKind CommandKind(int commandId)
    {
        switch (commandId)
        {
            case MemoryLayout:
                return HlslCommandKind.MemoryLayout;
            case SelectVariant:
                return HlslCommandKind.SelectVariant;
            case Compilation:
                return HlslCommandKind.Compilation;
            case ResourceBindings:
                return HlslCommandKind.ResourceBindings;
            case PreprocessorExplorer:
                return HlslCommandKind.PreprocessorExplorer;
            case EntryPointDataFlow:
                return HlslCommandKind.EntryPointDataFlow;
            case CallHierarchy:
                return HlslCommandKind.CallHierarchy;
            case ComputeVisualization:
                return HlslCommandKind.ComputeVisualization;
            case OpenEffectiveConfiguration:
                return HlslCommandKind.OpenEffectiveConfiguration;
            default:
                throw new ArgumentOutOfRangeException(
                    nameof(commandId),
                    commandId,
                    "Unknown HLSL command identifier.");
        }
    }
}

internal sealed class HlslCommandPresentation
{
    internal bool Visible { get; set; }

    internal bool Enabled { get; set; }

    internal string Text { get; set; }

    internal static HlslCommandPresentation Evaluate(
        HlslCommandKind kind,
        bool hlslEditor,
        bool contextKnown,
        HlslCommandContextModel context)
    {
        var text = BaseText(kind);
        if (!hlslEditor)
        {
            return new HlslCommandPresentation { Text = text };
        }
        if (IsDocumentCommand(kind))
        {
            return new HlslCommandPresentation
            {
                Visible = true,
                Enabled = true,
                Text = text,
            };
        }
        if (!contextKnown || context == null)
        {
            return new HlslCommandPresentation
            {
                Visible = true,
                Enabled = false,
                Text = text,
            };
        }

        var available = IsAvailable(kind, context);
        return new HlslCommandPresentation
        {
            Visible = available,
            Enabled = available,
            Text = available ? ContextText(kind, context) : text,
        };
    }

    private static bool IsDocumentCommand(HlslCommandKind kind)
        => kind == HlslCommandKind.SelectVariant ||
           kind == HlslCommandKind.OpenEffectiveConfiguration ||
           kind == HlslCommandKind.Compilation ||
           kind == HlslCommandKind.ResourceBindings ||
           kind == HlslCommandKind.PreprocessorExplorer;

    private static bool IsAvailable(
        HlslCommandKind kind,
        HlslCommandContextModel context)
    {
        switch (kind)
        {
            case HlslCommandKind.MemoryLayout:
                return context.MemoryLayoutAvailable;
            case HlslCommandKind.EntryPointDataFlow:
                return context.EntryPointDataFlowAvailable;
            case HlslCommandKind.CallHierarchy:
                return context.CallHierarchyAvailable;
            case HlslCommandKind.ComputeVisualization:
                return context.ComputeVisualizationAvailable;
            default:
                return true;
        }
    }

    private static string ContextText(
        HlslCommandKind kind,
        HlslCommandContextModel context)
    {
        switch (kind)
        {
            case HlslCommandKind.MemoryLayout:
                return WithTarget("Memory Layout", context.MemoryLayoutTarget);
            case HlslCommandKind.EntryPointDataFlow:
                return WithTarget("Entry-Point Data Flow", context.EntryPoint);
            case HlslCommandKind.CallHierarchy:
                return WithTarget("Call Hierarchy", context.CallableName);
            case HlslCommandKind.ComputeVisualization:
                return WithTarget("Compute Visualization", context.EntryPoint);
            default:
                return BaseText(kind);
        }
    }

    private static string WithTarget(string text, string target)
        => string.IsNullOrWhiteSpace(target) ? text : $"{text} for {target}";

    private static string BaseText(HlslCommandKind kind)
    {
        switch (kind)
        {
            case HlslCommandKind.MemoryLayout:
                return "Memory Layout";
            case HlslCommandKind.SelectVariant:
                return "Select Shader Variant";
            case HlslCommandKind.OpenEffectiveConfiguration:
                return "Open Effective Configuration";
            case HlslCommandKind.Compilation:
                return "Shader Compilation";
            case HlslCommandKind.ResourceBindings:
                return "Resource Bindings";
            case HlslCommandKind.PreprocessorExplorer:
                return "Preprocessor Explorer";
            case HlslCommandKind.EntryPointDataFlow:
                return "Entry-Point Data Flow";
            case HlslCommandKind.CallHierarchy:
                return "Call Hierarchy";
            case HlslCommandKind.ComputeVisualization:
                return "Compute Visualization";
            default:
                throw new ArgumentOutOfRangeException(nameof(kind));
        }
    }
}

public static class HlslCommandContextBridge
{
    private static Func<Uri, int, int, CancellationToken, Task<HlslCommandContextModel>> request;
    private static int revision;

    internal static event Action Invalidated;

    internal static int Revision => Volatile.Read(ref revision);

    public static void Register(
        Func<Uri, int, int, CancellationToken, Task<HlslCommandContextModel>> handler)
    {
        Volatile.Write(
            ref request,
            handler ?? throw new ArgumentNullException(nameof(handler)));
        Invalidate();
    }

    public static Task<HlslCommandContextModel> RequestAsync(
        Uri uri,
        int line,
        int character,
        CancellationToken cancellationToken)
    {
        var handler = Volatile.Read(ref request);
        return handler == null
            ? Task.FromResult<HlslCommandContextModel>(null)
            : handler(uri, line, character, cancellationToken);
    }

    public static void Invalidate()
    {
        Interlocked.Increment(ref revision);
        HlslCommandContextCache.Clear();
        Invalidated?.Invoke();
    }
}

internal static class HlslCommandContextCache
{
    private sealed class Entry
    {
        internal Entry(int line, int character, HlslCommandContextModel context)
        {
            Line = line;
            Character = character;
            Context = context;
        }

        internal int Line { get; }

        internal int Character { get; }

        internal HlslCommandContextModel Context { get; }
    }

    private static readonly object Gate = new();
    private static readonly Dictionary<object, (string Uri, Entry Context)> Entries = new();
    private static long revision;

    internal static long Revision
    {
        get
        {
            lock (Gate)
            {
                return revision;
            }
        }
    }

    internal static void Publish(
        object owner,
        Uri uri,
        int line,
        int character,
        HlslCommandContextModel context)
    {
        if (owner == null || uri == null || context == null)
        {
            return;
        }
        lock (Gate)
        {
            Entries[owner] = (uri.AbsoluteUri, new Entry(line, character, context));
        }
    }

    internal static bool PublishIfCurrent(
        long expectedRevision,
        object owner,
        Uri uri,
        int line,
        int character,
        HlslCommandContextModel context)
    {
        if (owner == null || uri == null || context == null)
        {
            return false;
        }
        lock (Gate)
        {
            if (revision != expectedRevision)
            {
                return false;
            }
            Entries[owner] = (uri.AbsoluteUri, new Entry(line, character, context));
            return true;
        }
    }

    internal static bool TryGet(
        Uri uri,
        int line,
        int character,
        out HlslCommandContextModel context)
    {
        context = null;
        if (uri == null)
        {
            return false;
        }
        lock (Gate)
        {
            foreach (var cached in Entries.Values)
            {
                if (string.Equals(
                        cached.Uri,
                        uri.AbsoluteUri,
                        StringComparison.OrdinalIgnoreCase) &&
                    cached.Context.Line == line &&
                    cached.Context.Character == character)
                {
                    context = cached.Context.Context;
                    return true;
                }
            }
            return false;
        }
    }

    internal static void Remove(object owner)
    {
        if (owner == null)
        {
            return;
        }
        lock (Gate)
        {
            Entries.Remove(owner);
        }
    }

    internal static void Clear()
    {
        lock (Gate)
        {
            Entries.Clear();
            ++revision;
        }
    }
}

internal sealed class HlslCommandContextTracker : IDisposable
{
    private static readonly TimeSpan RefreshDelay = TimeSpan.FromMilliseconds(125);

    private readonly IWpfTextView view;
    private readonly Uri documentUri;
    private readonly object cacheOwner = new();
    private CancellationTokenSource refreshCancellation;
    private Task refreshTask = Task.CompletedTask;
    private int requestGeneration;
    private bool disposed;

    private HlslCommandContextTracker(IWpfTextView view, string filePath)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        this.view = view;
        documentUri = new Uri(Path.GetFullPath(filePath));
        view.Caret.PositionChanged += OnPositionChanged;
        view.TextBuffer.Changed += OnBufferChanged;
        view.TextBuffer.ContentTypeChanged += OnContentTypeChanged;
        view.Closed += OnClosed;
        HlslCommandContextBridge.Invalidated += OnInvalidated;
        ScheduleRefresh();
    }

    internal static void Attach(IWpfTextView view, string filePath)
    {
        if (view == null || string.IsNullOrWhiteSpace(filePath))
        {
            return;
        }
        _ = new HlslCommandContextTracker(view, filePath);
    }

    public void Dispose()
    {
        if (disposed)
        {
            return;
        }
        disposed = true;
        view.Caret.PositionChanged -= OnPositionChanged;
        view.TextBuffer.Changed -= OnBufferChanged;
        view.TextBuffer.ContentTypeChanged -= OnContentTypeChanged;
        view.Closed -= OnClosed;
        HlslCommandContextBridge.Invalidated -= OnInvalidated;
        refreshCancellation?.Cancel();
        refreshCancellation?.Dispose();
        HlslCommandContextCache.Remove(cacheOwner);
    }

    private void OnPositionChanged(object sender, CaretPositionChangedEventArgs eventArgs)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        _ = sender;
        _ = eventArgs;
        ScheduleRefresh();
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

    private void OnClosed(object sender, EventArgs eventArgs)
    {
        _ = sender;
        _ = eventArgs;
        Dispose();
    }

    private void OnInvalidated()
    {
        HlslBootstrapPackage.RunOnMainThread(ScheduleRefresh);
    }

    private void ScheduleRefresh()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        HlslCommandContextCache.Remove(cacheOwner);
        refreshCancellation?.Cancel();
        refreshCancellation?.Dispose();
        refreshCancellation = null;
        if (disposed || !IsHlsl(view.TextBuffer))
        {
            return;
        }

        var position = view.Caret.Position.BufferPosition;
        var line = position.GetContainingLine();
        var lineNumber = line.LineNumber;
        var character = position.Position - line.Start.Position;
        var cancellation = new CancellationTokenSource();
        refreshCancellation = cancellation;
        var bridgeRevision = HlslCommandContextBridge.Revision;
        var cacheRevision = HlslCommandContextCache.Revision;
        var generation = ++requestGeneration;
        refreshTask = ResolveContextAsync(
            cancellation,
            bridgeRevision,
            cacheRevision,
            generation,
            lineNumber,
            character);
    }

    private async Task ResolveContextAsync(
        CancellationTokenSource cancellation,
        long bridgeRevision,
        long cacheRevision,
        int generation,
        int lineNumber,
        int character)
    {
        try
        {
            await Task.Delay(RefreshDelay, cancellation.Token)
                .ConfigureAwait(false);
            var context = await HlslCommandContextBridge.RequestAsync(
                    documentUri,
                    lineNumber,
                    character,
                    cancellation.Token)
                .ConfigureAwait(false);
            if (!cancellation.IsCancellationRequested &&
                generation == Volatile.Read(ref requestGeneration) &&
                bridgeRevision == HlslCommandContextBridge.Revision)
            {
                HlslCommandContextCache.PublishIfCurrent(
                    cacheRevision,
                    cacheOwner,
                    documentUri,
                    lineNumber,
                    character,
                    context);
            }
        }
        catch (OperationCanceledException)
        {
        }
        catch (Exception error)
        {
            ActivityLog.LogWarning(
                nameof(HlslCommandContextTracker),
                "Could not resolve HLSL command context: " + error.Message);
        }
    }

    private static bool IsHlsl(ITextBuffer buffer)
        => buffer.ContentType.IsOfType("HLSL") ||
           buffer.ContentType.IsOfType("HLSLHeader") ||
           buffer.ContentType.IsOfType("HLSL-LSP-Colored") ||
           buffer.ContentType.IsOfType("HLSLHeader-LSP-Colored");
}
