using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.Editor;
using Microsoft.VisualStudio.Language.Intellisense;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;
using Microsoft.VisualStudio.Text;
using Microsoft.VisualStudio.Text.Editor;
using Microsoft.VisualStudio.TextManager.Interop;
using Microsoft.VisualStudio.Threading;
using Newtonsoft.Json.Linq;

namespace HlslLsp.VisualStudio;

internal sealed class HlslNavigationBarClient :
    IVsDropdownBarClient,
    IVsDropdownBarClient3,
    IVsDropdownBarClientEx,
    IVsCoTaskMemFreeMyStrings,
    IDisposable
{
    private IVsDropdownBarManager manager;
    private readonly IVsEditorAdaptersFactoryService editorAdapters;
    private readonly HlslLanguageClient languageClient;
    private readonly JoinableTaskFactory joinableTaskFactory;
    private readonly Action<HlslNavigationBarClient> closed;
    private readonly IntPtr imageList;
    private readonly List<NavigationItem> scopes = new();
    private ITextBuffer buffer;
    private IWpfTextView view;
    private Uri documentUri;
    private CancellationTokenSource updateCancellation = new();
    private IVsDropdownBar dropdownBar;
    private bool disposed;

    internal HlslNavigationBarClient(
        IVsDropdownBarManager manager,
        IVsCodeWindow codeWindow,
        ITextBuffer buffer,
        IVsEditorAdaptersFactoryService editorAdapters,
        HlslLanguageClient languageClient,
        JoinableTaskFactory joinableTaskFactory,
        IServiceProvider serviceProvider,
        string filePath,
        Action<HlslNavigationBarClient> closed)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        this.manager = manager;
        this.editorAdapters = editorAdapters;
        this.buffer = buffer;
        this.languageClient = languageClient;
        this.joinableTaskFactory = joinableTaskFactory;
        this.closed = closed;
        documentUri = new Uri(filePath);

        codeWindow.GetPrimaryView(out var textView);
        view = editorAdapters.GetWpfTextView(textView);
        if (view == null)
        {
            throw new InvalidOperationException(
                "Visual Studio's HLSL text view is unavailable.");
        }

        if (serviceProvider.GetService(typeof(SVsShell)) is IVsShell shell &&
            ErrorHandler.Succeeded(
                shell.GetProperty(
                    (int)__VSSPROPID.VSSPROPID_ObjectMgrTypesImgList,
                    out var rawImageList)) &&
            rawImageList != null)
        {
            imageList = new IntPtr(Convert.ToInt64(rawImageList));
        }

        buffer.Changed += OnBufferChanged;
        view.Caret.PositionChanged += OnCaretPositionChanged;
        view.Closed += OnViewClosed;
    }

    internal void Refresh()
    {
        var cancellation = ReplaceUpdateCancellation();
        joinableTaskFactory.RunAsync(
                () => RefreshAndReportAsync(cancellation.Token))
            .FileAndForget("HlslLsp/RefreshNavigationBar");
    }

    internal bool Matches(ITextBuffer candidateBuffer, string filePath) =>
        ReferenceEquals(buffer, candidateBuffer) &&
        documentUri == new Uri(filePath);

    internal bool HasDropdownBar => dropdownBar != null;

    internal void Detach()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        manager.RemoveDropdownBar();
        dropdownBar = null;
    }

    internal void Activate(IVsDropdownBarManager candidateManager)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        manager = candidateManager;
        RefreshSelections();
    }

    internal void Rebind(
        IVsDropdownBarManager candidateManager,
        IVsCodeWindow codeWindow,
        ITextBuffer candidateBuffer,
        string filePath)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        codeWindow.GetPrimaryView(out var textView);
        var candidateView = editorAdapters.GetWpfTextView(textView);
        if (candidateView == null)
        {
            throw new InvalidOperationException(
                "Visual Studio's HLSL text view is unavailable.");
        }

        buffer.Changed -= OnBufferChanged;
        view.Caret.PositionChanged -= OnCaretPositionChanged;
        view.Closed -= OnViewClosed;
        buffer = candidateBuffer;
        view = candidateView;
        documentUri = new Uri(filePath);
        manager = candidateManager;
        buffer.Changed += OnBufferChanged;
        view.Caret.PositionChanged += OnCaretPositionChanged;
        view.Closed += OnViewClosed;
        Refresh();
    }

    private async Task RefreshAndReportAsync(CancellationToken cancellationToken)
    {
        try
        {
            await RefreshAsync(cancellationToken);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
        }
        catch (Exception error)
        {
            ActivityLog.LogError(
                nameof(HlslNavigationBarClient),
                error.ToString());
        }
    }

    private CancellationTokenSource ReplaceUpdateCancellation()
    {
        var replacement = new CancellationTokenSource();
        var previous = Interlocked.Exchange(
            ref updateCancellation,
            replacement);
        previous.Cancel();
        previous.Dispose();
        return replacement;
    }

    private async Task RefreshAsync(CancellationToken cancellationToken)
    {
        var symbols = await languageClient.GetDocumentSymbolsAsync(
                documentUri,
                cancellationToken)
            .ConfigureAwait(false);
        var updatedScopes = CreateScopes(symbols);

        await joinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (disposed)
        {
            return;
        }
        scopes.Clear();
        scopes.AddRange(updatedScopes);
        RefreshSelections();
    }

    private static List<NavigationItem> CreateScopes(JArray symbols)
    {
        var global = new NavigationItem(
            "(Global Scope)",
            2,
            default,
            default);
        var result = new List<NavigationItem> { global };
        foreach (var symbol in symbols.OfType<JObject>())
        {
            AddSymbol(symbol, null, global, result);
        }
        foreach (var duplicates in result
                     .GroupBy(item => item.Text)
                     .Where(group => group.Count() > 1))
        {
            foreach (var item in duplicates)
            {
                item.Text += $" (line {item.SelectionStart.Line + 1})";
            }
        }
        return result;
    }

    private static void AddSymbol(
        JObject symbol,
        string containerName,
        NavigationItem global,
        List<NavigationItem> scopes)
    {
        var item = NavigationItem.FromJson(symbol);
        if (IsContainer(item.Kind))
        {
            item.Text = string.IsNullOrEmpty(containerName)
                ? item.Text
                : containerName + "::" + item.Text;
            scopes.Add(item);
            foreach (var child in symbol["children"]?.OfType<JObject>() ??
                Enumerable.Empty<JObject>())
            {
                var childItem = NavigationItem.FromJson(child);
                if (IsContainer(childItem.Kind))
                {
                    item.Children.Add(childItem);
                    AddSymbol(child, item.Text, global, scopes);
                }
                else
                {
                    item.Children.Add(childItem);
                }
            }
        }
        else
        {
            global.Children.Add(item);
        }
    }

    private static bool IsContainer(int kind) =>
        kind == 3 || kind == 5 || kind == 10 || kind == 11 || kind == 23;

    private void OnBufferChanged(object sender, TextContentChangedEventArgs eventArgs)
    {
        var cancellation = ReplaceUpdateCancellation();
        joinableTaskFactory.RunAsync(
                async () =>
                {
                    await Task.Delay(300, cancellation.Token);
                    await RefreshAsync(cancellation.Token);
                })
            .FileAndForget("HlslLsp/RefreshNavigationBarAfterEdit");
    }

    private void OnCaretPositionChanged(
        object sender,
        CaretPositionChangedEventArgs eventArgs)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        RefreshSelections();
    }

    private void OnViewClosed(object sender, EventArgs eventArgs)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        Dispose();
        closed(this);
    }

    private void RefreshSelections()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (dropdownBar == null || scopes.Count == 0)
        {
            return;
        }

        var position = view.Caret.Position.BufferPosition.Position;
        var scopeIndex = 0;
        for (var index = 1; index < scopes.Count; ++index)
        {
            if (Contains(scopes[index], position))
            {
                scopeIndex = index;
            }
        }
        var memberIndex = -1;
        var members = scopes[scopeIndex].Children;
        for (var index = 0; index < members.Count; ++index)
        {
            if (Contains(members[index], position))
            {
                memberIndex = index;
                break;
            }
        }
        if (memberIndex < 0 && scopes[scopeIndex].Kind == 3)
        {
            scopeIndex = 0;
        }

        dropdownBar.RefreshCombo(0, scopeIndex);
        dropdownBar.RefreshCombo(1, memberIndex);
    }

    private bool Contains(NavigationItem item, int position)
    {
        var snapshot = buffer.CurrentSnapshot;
        return ToPosition(snapshot, item.RangeStart) <= position &&
               position <= ToPosition(snapshot, item.RangeEnd);
    }

    private static int ToPosition(ITextSnapshot snapshot, TextPosition position)
    {
        if (snapshot.LineCount == 0)
        {
            return 0;
        }
        var lineNumber = Math.Min(
            Math.Max(position.Line, 0),
            snapshot.LineCount - 1);
        var line = snapshot.GetLineFromLineNumber(lineNumber);
        return Math.Min(
            line.Start.Position + Math.Max(position.Character, 0),
            line.End.Position);
    }

    private NavigationItem GetItem(int combo, int index)
    {
        if (combo == 0)
        {
            return scopes[index];
        }
        dropdownBar.GetCurrentSelection(0, out var scopeIndex);
        return scopes[Math.Max(scopeIndex, 0)].Children[index];
    }

    private void NavigateTo(NavigationItem item)
    {
        var position = ToPosition(
            buffer.CurrentSnapshot,
            item.SelectionStart);
        view.Caret.MoveTo(new SnapshotPoint(buffer.CurrentSnapshot, position));
        view.ViewScroller.EnsureSpanVisible(
            new SnapshotSpan(buffer.CurrentSnapshot, position, 0));
        view.VisualElement.Focus();
    }

    public int GetComboAttributes(
        int combo,
        out uint entryCount,
        out uint entryType,
        out IntPtr images)
    {
        entryType = (uint)(
            DROPDOWNENTRYTYPE.ENTRY_TEXT |
            DROPDOWNENTRYTYPE.ENTRY_ATTR |
            DROPDOWNENTRYTYPE.ENTRY_IMAGE);
        images = imageList;
        if (combo == 0)
        {
            entryCount = (uint)scopes.Count;
            return VSConstants.S_OK;
        }
        if (combo == 1)
        {
            var scopeIndex = 0;
            dropdownBar?.GetCurrentSelection(0, out scopeIndex);
            entryCount = scopes.Count == 0
                ? 0
                : (uint)scopes[Math.Max(scopeIndex, 0)].Children.Count;
            return VSConstants.S_OK;
        }
        entryCount = 0;
        return VSConstants.E_INVALIDARG;
    }

    public int GetComboTipText(int combo, out string text)
    {
        text = combo == 0
            ? "Navigate to an HLSL scope"
            : "Navigate to an HLSL symbol";
        return combo is 0 or 1
            ? VSConstants.S_OK
            : VSConstants.E_INVALIDARG;
    }

    public int GetEntryAttributes(int combo, int index, out uint attributes)
    {
        attributes = (uint)DROPDOWNFONTATTR.FONTATTR_PLAIN;
        return VSConstants.S_OK;
    }

    public int GetEntryImage(int combo, int index, out int imageIndex)
    {
        imageIndex = GetItem(combo, index).ImageIndex;
        return VSConstants.S_OK;
    }

    public int GetEntryText(int combo, int index, out string text)
    {
        text = GetItem(combo, index).Text;
        return VSConstants.S_OK;
    }

    public int OnComboGetFocus(int combo) => VSConstants.S_OK;

    public int OnItemChosen(int combo, int index)
    {
        if (combo == 0)
        {
            dropdownBar.RefreshCombo(1, -1);
        }
        NavigateTo(GetItem(combo, index));
        return VSConstants.S_OK;
    }

    public int OnItemSelected(int combo, int index) => VSConstants.S_OK;

    public int SetDropdownBar(IVsDropdownBar value)
    {
        dropdownBar = value;
        Refresh();
        return VSConstants.S_OK;
    }

    public int GetComboWidth(int combo, out int widthPercent)
    {
        widthPercent = 100;
        return VSConstants.S_OK;
    }

    public int GetAutomationProperties(
        int combo,
        out string name,
        out string automationId)
    {
        name = combo == 0 ? "HLSL scopes" : "HLSL symbols";
        automationId = combo == 0
            ? "HlslNavigationScopes"
            : "HlslNavigationSymbols";
        return combo is 0 or 1
            ? VSConstants.S_OK
            : VSConstants.E_INVALIDARG;
    }

    public int GetEntryImage(
        int combo,
        int index,
        out int imageIndex,
        out IntPtr images)
    {
        imageIndex = GetItem(combo, index).ImageIndex;
        images = imageList;
        return VSConstants.S_OK;
    }

    public int GetEntryIndent(int combo, int index, out uint indent)
    {
        indent = 0;
        return VSConstants.S_OK;
    }

    public void Dispose()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (disposed)
        {
            return;
        }
        disposed = true;
        buffer.Changed -= OnBufferChanged;
        view.Caret.PositionChanged -= OnCaretPositionChanged;
        view.Closed -= OnViewClosed;
        updateCancellation.Cancel();
        updateCancellation.Dispose();
        manager.RemoveDropdownBar();
    }

    private readonly struct TextPosition
    {
        internal TextPosition(int line, int character)
        {
            Line = line;
            Character = character;
        }

        internal int Line { get; }
        internal int Character { get; }
    }

    private sealed class NavigationItem
    {
        internal NavigationItem(
            string text,
            int kind,
            TextPosition rangeStart,
            TextPosition rangeEnd)
        {
            Text = text;
            Kind = kind;
            RangeStart = rangeStart;
            RangeEnd = rangeEnd;
            SelectionStart = rangeStart;
        }

        internal string Text { get; set; }
        internal int Kind { get; }
        internal TextPosition RangeStart { get; }
        internal TextPosition RangeEnd { get; }
        internal TextPosition SelectionStart { get; private set; }
        internal List<NavigationItem> Children { get; } = new();
        internal int ImageIndex => GlyphIndex(Kind);

        internal static NavigationItem FromJson(JObject value)
        {
            var range = value["range"];
            var selection = value["selectionRange"];
            var item = new NavigationItem(
                (string)value["name"] ?? string.Empty,
                (int?)value["kind"] ?? 13,
                Position(range?["start"]),
                Position(range?["end"]));
            item.SelectionStart = Position(selection?["start"]);
            return item;
        }

        private static TextPosition Position(JToken value) =>
            new(
                (int?)value?["line"] ?? 0,
                (int?)value?["character"] ?? 0);

        private static int GlyphIndex(int kind)
        {
            var group = kind switch
            {
                3 => StandardGlyphGroup.GlyphGroupNamespace,
                5 => StandardGlyphGroup.GlyphGroupClass,
                6 or 9 or 12 => StandardGlyphGroup.GlyphGroupMethod,
                7 or 8 => StandardGlyphGroup.GlyphGroupField,
                10 or 22 => StandardGlyphGroup.GlyphGroupEnum,
                11 => StandardGlyphGroup.GlyphGroupInterface,
                14 => StandardGlyphGroup.GlyphGroupConstant,
                23 => StandardGlyphGroup.GlyphGroupStruct,
                25 => StandardGlyphGroup.GlyphGroupOperator,
                26 => StandardGlyphGroup.GlyphGroupType,
                _ => StandardGlyphGroup.GlyphGroupVariable,
            };
            return (int)group + (int)StandardGlyphItem.GlyphItemPublic;
        }
    }
}
