using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Media;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;
using Microsoft.VisualStudio.Text;
using Microsoft.VisualStudio.TextManager.Interop;

namespace HlslLsp.VisualStudio.Bootstrap;

// A dedicated **HLSL Call Hierarchy** tool window: the custom Visual Studio
// surface for the standard LSP call hierarchy (textDocument/
// prepareCallHierarchy, callHierarchy/incomingCalls,
// callHierarchy/outgoingCalls -- see docs/call-hierarchy.md). This exists
// because Visual Studio 17.14's generic ILanguageClient/
// Microsoft.VisualStudio.LanguageServer.Client infrastructure does not
// route the editor's built-in View Call Hierarchy command to a language
// client that merely advertises callHierarchyProvider: true -- there is no
// bespoke call-hierarchy surface in that SDK the way there is for hover,
// signature help, or go-to-definition. A prior version of this client's
// documentation assumed otherwise; this tool window and command are the
// correction. Requests its own three RPCs through CallHierarchyBridge,
// mirroring EntryPointDataFlowToolWindow's/PreprocessorExplorerToolWindow's
// structure against distinct protocol requests.
[Guid("6f2e6f36-9a3d-4e6a-9f0b-2f7f8f0b9a1c")]
public sealed class CallHierarchyExplorerToolWindow : ToolWindowPane
{
    private readonly CallHierarchyExplorerControl control = new();
    private readonly CallHierarchyExplorerState state = new();
    private bool wired;
    private string banner;
    private string placeholderMessage =
        "Open an HLSL document, place the caret on a function, then run " +
        "Tools > HLSL Call Hierarchy.";

    public CallHierarchyExplorerToolWindow()
        : base(null)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        Caption = "HLSL Call Hierarchy";
        Content = control;
        Render();
    }

    // The callable item currently centered in the view (root, or a
    // drilled-into caller/callee), with its opaque data envelope intact --
    // used by HlslBootstrapPackage to re-issue incomingCalls/outgoingCalls
    // for a background refresh (save/variant change) without needing a
    // fresh prepareCallHierarchy call. Null when no callable has ever been
    // resolved yet (placeholder or not-callable state).
    internal CallHierarchyItemModel CurrentItem => state.Current?.Item;

    // The document/caret position textDocument/prepareCallHierarchy was
    // last (successfully) run at to establish the current root -- retained
    // so a background refresh can re-run prepareCallHierarchy from scratch
    // rather than trusting CurrentItem's own (possibly now-stale) opaque
    // data directly against incomingCalls/outgoingCalls, which would be
    // guaranteed to fail with ContentModified once the root's generation
    // has actually changed (see docs/call-hierarchy.md). Null when no root
    // has ever been established, or after an authoritative "not callable"/
    // global-error result, since there is then nothing meaningful left to
    // refresh from.
    internal Uri RootDocumentUri { get; private set; }

    internal int RootLine { get; private set; }

    internal int RootCharacter { get; private set; }

    // A live anchor for RootLine/RootCharacter in the buffer that was open
    // when the root was established, tracking edits made anywhere before
    // it (see CallHierarchyRootPositionResolver). Null whenever no live
    // buffer was available to anchor against (never fatal -- callers fall
    // back to the root item's own compiler-supplied SelectionRange.Start,
    // then finally to the raw RootLine/RootCharacter captured above).
    // ITrackingPoint has no Dispose contract of its own; "detaching" it
    // simply means dropping this reference, which every path that changes
    // or clears the root (SetRoot/UpdateRootAnchor/SetNotCallable/
    // SetGlobalError) already does by overwriting or nulling this property.
    internal ITrackingPoint RootTrackingPoint { get; private set; }

    // The root frame's own item (bottom of the drill-in stack), distinct
    // from CurrentItem (the top of the stack): used as the compiler-
    // verified fallback location described above. Null under exactly the
    // same conditions as RootDocumentUri.
    internal CallHierarchyItemModel RootItem => state.Root?.Item;

    // Bumped whenever the navigation stack's shape or position changes
    // (root established/reset, drilled in, went back, or a background
    // refresh replaced the whole stack). An in-flight asynchronous
    // operation that captured this value before starting can compare it
    // again immediately before committing its own result and discard that
    // result if the user navigated in the meantime -- see
    // HlslBootstrapPackage.RefreshCallHierarchyIfOpenAsync/
    // PerformCallHierarchyDrillInAsync.
    internal long NavigationRevision => state.Revision;

    // The current drill-in path (empty when showing only the root), used
    // by a background refresh to attempt to relocate the user's position
    // underneath a freshly re-resolved root (see CallHierarchyItemIdentity).
    internal IReadOnlyList<CallHierarchyPathStep> CapturePathSteps() => state.CapturePath();

    // Wires the control's Back/drill-in interactions to package-owned
    // callbacks exactly once per window instance: ShowToolWindowAsync/
    // FindToolWindowAsync return the same singleton pane across every
    // invocation of the Tools command, so re-wiring on each call would
    // double- (then triple-, ...) subscribe the same event without this
    // guard. onDrillIn is invoked with the clicked caller/callee item and
    // which section (incoming callers vs. outgoing callees) it was found
    // in; the package owns issuing the resulting incomingCalls/
    // outgoingCalls request (generation guarding, cancellation, and the
    // explicit-request gate all belong there, matching every other request
    // in this package). Back never needs the package at all -- the
    // previous frame was already fetched, so popping the stack is a pure,
    // immediate operation; it deliberately does NOT clear an existing
    // stale/error banner (see BackRequested below).
    internal void EnsureWired(Action<CallHierarchyItemModel, CallHierarchySection> onDrillIn)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (wired)
        {
            return;
        }
        wired = true;
        control.DrillInRequested += (item, section) => onDrillIn(item, section);
        control.BackRequested += () =>
        {
            ThreadHelper.ThrowIfNotOnUIThread();
            // A banner set because the current root/path is stale or a
            // refresh failed describes the *root*, not any one frame --
            // every frame in the stack was derived from the same root
            // generation, so going back must never silently imply an
            // ancestor frame is somehow more current. The banner is only
            // ever cleared by a genuinely fresh root/path resolution
            // (SetRoot/PushFrame/ReplaceAllFrames), never by navigation.
            if (state.GoBack())
            {
                Render();
            }
        };
    }

    // The caret did not resolve to a callable declaration (a standard,
    // non-error LSP result -- see docs/call-hierarchy.md,
    // "textDocument/prepareCallHierarchy") or the document was not open at
    // all. This is an authoritative result, not a transient failure, so it
    // always replaces whatever was shown before, exactly like
    // EntryPointDataFlowToolWindow's not-found handling.
    internal void SetNotCallable()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        state.Clear();
        banner = null;
        RootDocumentUri = null;
        RootLine = 0;
        RootCharacter = 0;
        RootTrackingPoint = null;
        placeholderMessage = CallHierarchyExplorerDisplay.NoCallableMessage();
        Render();
    }

    // A brand-new root was resolved (an explicit Tools-command invocation,
    // or a background refresh that had to fall back to a fresh root):
    // resets any prior drill-in stack and records the position it was
    // resolved from so a future background refresh can re-run
    // prepareCallHierarchy from the same place. trackingPoint anchors that
    // position in the buffer that was live when it was resolved, if one
    // was available (see RootTrackingPoint); null is always safe and only
    // means a future refresh falls back to RootItem's own SelectionRange.
    internal void SetRoot(
        Uri documentUri,
        int line,
        int character,
        CallHierarchyFrame frame,
        ITrackingPoint trackingPoint = null)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        state.Reset(frame);
        banner = null;
        RootDocumentUri = documentUri;
        RootLine = line;
        RootCharacter = character;
        RootTrackingPoint = trackingPoint;
        Render();
    }

    // Updates only the root anchor metadata (document/position/tracking
    // point) without touching the navigation stack or banner -- used after
    // a background refresh successfully rebuilds the user's full drill-in
    // path underneath a freshly re-resolved root (see ReplaceAllFrames):
    // the stack itself is replaced separately, but the anchor a *future*
    // refresh should re-track from must still move forward to the position
    // this refresh actually resolved, not remain pinned to a now-outdated
    // one.
    internal void UpdateRootAnchor(
        Uri documentUri,
        int line,
        int character,
        ITrackingPoint trackingPoint)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        RootDocumentUri = documentUri;
        RootLine = line;
        RootCharacter = character;
        RootTrackingPoint = trackingPoint;
    }

    // The user drilled into a caller/callee: pushes a new frame on top of
    // the existing stack so Back can return to it. section records which
    // of the parent frame's two lists item was found in, so a later
    // background refresh can attempt to relocate it underneath a freshly
    // re-resolved root (see CallHierarchyItemIdentity).
    internal void PushFrame(CallHierarchyItemModel item, CallHierarchySection section, CallHierarchyFrame frame)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        state.Push(frame, CallHierarchyItemIdentity.CapturePathStep(section, item));
        banner = null;
        Render();
    }

    // A background refresh (save/variant change) successfully re-fetched
    // incoming/outgoing calls for the same current item: replaces its
    // content in place without disturbing the drill-in stack depth, and
    // clears any earlier stale/error banner.
    internal void ReplaceCurrentFrame(CallHierarchyFrame frame)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        state.ReplaceCurrent(frame);
        banner = null;
        Render();
    }

    // A background refresh successfully re-resolved the root from scratch
    // and relocated every frame the user had drilled into underneath it:
    // replaces the entire stack in one step (rather than the current frame
    // alone), preserving the user's drill-in depth, and clears any earlier
    // stale/error banner since this is a fully fresh, successful result.
    internal void ReplaceAllFrames(
        IReadOnlyList<CallHierarchyFrame> frames,
        IReadOnlyList<CallHierarchyPathStep> steps)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        state.ReplaceAll(frames, steps);
        banner = null;
        Render();
    }

    // A background refresh found the current item is now stale (server
    // returned ContentModified -- see docs/call-hierarchy.md) or failed
    // transiently: keeps the last successfully fetched content on screen
    // exactly as before and overlays an explanatory banner rather than
    // erasing it, matching EntryPointDataFlowToolWindow's "preserve last
    // successful content on transient errors" contract.
    internal void SetBannerOnCurrent(string message)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        banner = message;
        Render();
    }

    // No content has ever been successfully shown yet (e.g. the very first
    // explicit invocation's own request failed outright): unlike
    // SetBannerOnCurrent, there is nothing to preserve, so this replaces
    // the placeholder text itself with the failure reason.
    internal void SetGlobalError(string message)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        state.Clear();
        banner = null;
        RootDocumentUri = null;
        RootLine = 0;
        RootCharacter = 0;
        RootTrackingPoint = null;
        placeholderMessage = message;
        Render();
    }

    private void Render()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        control.Render(state.Current, state.CanGoBack, banner, placeholderMessage);
    }
}

internal sealed class CallHierarchyExplorerControl : UserControl
{
    private readonly StackPanel content = new();

    internal event Action<CallHierarchyItemModel, CallHierarchySection> DrillInRequested;

    internal event Action BackRequested;

    internal CallHierarchyExplorerControl()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        Content = new ScrollViewer
        {
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
            Content = content,
        };
        Render(null, false, null, "Open an HLSL document, place the caret on a function, then run Tools > HLSL Call Hierarchy.");
    }

    internal void Render(CallHierarchyFrame frame, bool canGoBack, string banner, string placeholderMessage)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        content.Children.Clear();
        content.Margin = new Thickness(12);
        content.Children.Add(new TextBlock
        {
            Text = "Call Hierarchy",
            FontSize = 18,
            FontWeight = FontWeights.SemiBold,
        });

        if (frame == null)
        {
            content.Children.Add(new TextBlock
            {
                Text = placeholderMessage ?? string.Empty,
                Foreground = Brushes.Goldenrod,
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 6, 0, 0),
            });
            return;
        }

        if (canGoBack)
        {
            var back = new TextBlock { Margin = new Thickness(0, 6, 0, 0) };
            var backLink = new Hyperlink(new Run("\u2190 Back")) { ToolTip = "Return to the previous call hierarchy view" };
            backLink.Click += (_, _) => BackRequested?.Invoke();
            back.Inlines.Add(backLink);
            content.Children.Add(back);
        }

        AddSelectedHeader(frame.Item);

        if (!string.IsNullOrEmpty(banner))
        {
            content.Children.Add(new TextBlock
            {
                Text = banner,
                Foreground = Brushes.Goldenrod,
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 8, 0, 8),
            });
        }

        AddSection("Incoming calls (callers)", () => AddIncomingCalls(frame.Incoming));
        AddSection("Outgoing calls (callees)", () => AddOutgoingCalls(frame.Outgoing));
    }
    private void AddSelectedHeader(CallHierarchyItemModel item)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        var line = new TextBlock
        {
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(0, 10, 0, 4),
            FontSize = 14,
            FontWeight = FontWeights.SemiBold,
        };
        var link = new Hyperlink(new Run(FunctionLabel(item))) { ToolTip = "Go to declaration" };
        var navRange = EntryPointDataFlowNavigation.SelectNavigationRange(item?.Range, item?.SelectionRange);
        link.Click += (_, _) => NavigateTo(item?.Uri, navRange);
        line.Inlines.Add(link);
        content.Children.Add(line);
        if (!string.IsNullOrEmpty(item?.Detail) && !string.Equals(item.Detail, item.Name, StringComparison.Ordinal))
        {
            content.Children.Add(new TextBlock
            {
                Text = item.Detail,
                Opacity = 0.75,
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 0, 0, 4),
            });
        }
    }

    private void AddSection(string title, Action addBody)
    {
        content.Children.Add(new TextBlock
        {
            Text = title,
            FontSize = 13,
            FontWeight = FontWeights.SemiBold,
            Margin = new Thickness(0, 14, 0, 6),
        });
        addBody();
    }

    private void AddIncomingCalls(IReadOnlyList<CallHierarchyIncomingCallModel> incoming)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (incoming == null || incoming.Count == 0)
        {
            content.Children.Add(new TextBlock { Text = "(none)", Opacity = 0.75 });
            return;
        }
        foreach (var call in incoming)
        {
            AddCallRow(call?.From, call?.FromRanges?.Count ?? 0, CallHierarchySection.Incoming);
        }
    }

    private void AddOutgoingCalls(IReadOnlyList<CallHierarchyOutgoingCallModel> outgoing)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (outgoing == null || outgoing.Count == 0)
        {
            content.Children.Add(new TextBlock { Text = "(none)", Opacity = 0.75 });
            return;
        }
        foreach (var call in outgoing)
        {
            AddCallRow(call?.To, call?.FromRanges?.Count ?? 0, CallHierarchySection.Outgoing);
        }
    }

    // One caller/callee row: the item's own name/detail navigates to its
    // SelectionRange (falling back to Range only if absent/invalid, via the
    // shared EntryPointDataFlowNavigation helper -- never a location
    // inferred from the displayed name), a call-site count label, and a
    // separate "Explore" action that re-centers the whole view on this
    // item (drilling in) rather than merely navigating to it. section
    // records which list item was found in, so a later background refresh
    // can attempt to relocate it underneath a freshly re-resolved root.
    private void AddCallRow(CallHierarchyItemModel item, int fromRangeCount, CallHierarchySection section)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        var row = new WrapPanel { Margin = new Thickness(0, 0, 0, 6) };
        var nameBlock = new TextBlock { TextWrapping = TextWrapping.Wrap };
        var nameLink = new Hyperlink(new Run(FunctionLabel(item))) { ToolTip = "Go to declaration" };
        var navRange = EntryPointDataFlowNavigation.SelectNavigationRange(item?.Range, item?.SelectionRange);
        nameLink.Click += (_, _) => NavigateTo(item?.Uri, navRange);
        nameBlock.Inlines.Add(nameLink);
        row.Children.Add(nameBlock);

        var countLabel = CallHierarchyExplorerDisplay.CallSiteCountLabel(fromRangeCount);
        if (!string.IsNullOrEmpty(countLabel))
        {
            row.Children.Add(new TextBlock
            {
                Text = "  (" + countLabel + ")",
                Opacity = 0.65,
                Margin = new Thickness(4, 0, 0, 0),
            });
        }

        if (item != null)
        {
            var exploreBlock = new TextBlock { Margin = new Thickness(10, 0, 0, 0) };
            var exploreLink = new Hyperlink(new Run("Explore calls"))
            {
                ToolTip = "Show this item's own incoming/outgoing calls",
            };
            exploreLink.Click += (_, _) => DrillInRequested?.Invoke(item, section);
            exploreBlock.Inlines.Add(exploreLink);
            row.Children.Add(exploreBlock);
        }

        content.Children.Add(row);
    }

    private static string FunctionLabel(CallHierarchyItemModel item)
        => item == null
            ? string.Empty
            : string.IsNullOrEmpty(item.Name)
                ? item.Detail ?? string.Empty
                : item.Name;

    // --- Navigation --------------------------------------------------
    // Duplicated per tool window rather than shared, matching this
    // repository's existing convention (see EntryPointDataFlowToolWindow's
    // own NavigateTo/TryValidateLocation/ShowNavigationError, and its
    // comment referencing ResourceBindingsToolWindow/
    // PreprocessorExplorerToolWindow's equivalents) -- only the
    // Range-vs-SelectionRange preference itself is shared, via
    // EntryPointDataFlowNavigation above.

    private void NavigateTo(string uriValue, CompilationSourceRangeModel range)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (!TryValidateLocation(
                uriValue,
                range,
                out var filePath,
                out var startLine,
                out var startCharacter,
                out var endLine,
                out var endCharacter,
                out var validationError))
        {
            ShowNavigationError(validationError);
            return;
        }
        try
        {
            VsShellUtilities.OpenDocument(
                ServiceProvider.GlobalProvider,
                filePath,
                VSConstants.LOGVIEWID.TextView_guid,
                out _,
                out _,
                out var frame,
                out var view);
            frame?.Show();
            view?.SetSelection(startLine, startCharacter, endLine, endCharacter);
            view?.EnsureSpanVisible(new TextSpan
            {
                iStartLine = startLine,
                iStartIndex = startCharacter,
                iEndLine = endLine,
                iEndIndex = endCharacter,
            });
        }
        catch (Exception ex)
        {
            ShowNavigationError($"Unable to navigate to the call hierarchy location: {ex.Message}");
        }
    }

    private static void ShowNavigationError(string message)
    {
        VsShellUtilities.ShowMessageBox(
            ServiceProvider.GlobalProvider,
            message,
            "HLSL Call Hierarchy",
            OLEMSGICON.OLEMSGICON_WARNING,
            OLEMSGBUTTON.OLEMSGBUTTON_OK,
            OLEMSGDEFBUTTON.OLEMSGDEFBUTTON_FIRST);
    }

    private static bool TryValidateLocation(
        string uriValue,
        CompilationSourceRangeModel range,
        out string filePath,
        out int startLine,
        out int startCharacter,
        out int endLine,
        out int endCharacter,
        out string error)
    {
        filePath = null;
        startLine = startCharacter = endLine = endCharacter = 0;
        error = null;
        if (string.IsNullOrEmpty(uriValue))
        {
            error = "The call hierarchy location was missing.";
            return false;
        }
        Uri uri;
        try
        {
            uri = new Uri(uriValue, UriKind.Absolute);
        }
        catch (UriFormatException)
        {
            error = "The call hierarchy location's URI could not be parsed.";
            return false;
        }
        if (!uri.IsFile)
        {
            error = "The call hierarchy location does not refer to a local file.";
            return false;
        }
        if (range?.Start == null || range.End == null)
        {
            error = "The call hierarchy location's range was missing.";
            return false;
        }
        if (!TryToInt32(range.Start.Line, out startLine) ||
            !TryToInt32(range.Start.Character, out startCharacter) ||
            !TryToInt32(range.End.Line, out endLine) ||
            !TryToInt32(range.End.Character, out endCharacter))
        {
            error = "The call hierarchy location's range contained an invalid position.";
            return false;
        }
        if (endLine < startLine || (endLine == startLine && endCharacter < startCharacter))
        {
            error = "The call hierarchy location's range was inverted.";
            return false;
        }
        filePath = uri.LocalPath;
        if (!File.Exists(filePath))
        {
            error = $"The file \"{filePath}\" could not be found.";
            return false;
        }
        return true;
    }

    private static bool TryToInt32(long value, out int result)
    {
        if (value < 0 || value > int.MaxValue)
        {
            result = 0;
            return false;
        }
        result = (int)value;
        return true;
    }
}
