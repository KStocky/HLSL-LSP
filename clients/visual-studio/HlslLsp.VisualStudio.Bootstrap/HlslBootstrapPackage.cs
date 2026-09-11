using System;
using System.Collections.Generic;
using System.ComponentModel.Design;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.ComponentModelHost;
using Microsoft.VisualStudio.Editor;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;
using Microsoft.VisualStudio.Text;
using Microsoft.VisualStudio.TextManager.Interop;

namespace HlslLsp.VisualStudio.Bootstrap;

[PackageRegistration(UseManagedResourcesOnly = true, AllowsBackgroundLoading = true)]
[InstalledProductRegistration("HLSL-LSP", "DXC-powered HLSL IntelliSense", "0.14.1")]
[ProvideSettingsManifest(PackageRelativeManifestFile = "HlslLsp.registration.json")]
[ProvideMenuResource("Menus.ctmenu", 1)]
[ProvideToolWindow(typeof(MemoryLayoutToolWindow))]
[ProvideToolWindow(typeof(CompilationInfoToolWindow))]
[ProvideToolWindow(typeof(ResourceBindingsToolWindow))]
[ProvideToolWindow(typeof(PreprocessorExplorerToolWindow))]
[ProvideToolWindow(typeof(EntryPointDataFlowToolWindow))]
[ProvideToolWindow(typeof(ComputeVisualizationToolWindow))]
[ProvideToolWindow(typeof(CallHierarchyExplorerToolWindow))]
[ProvideOptionPage(
    typeof(HlslOptionsPage),
    "HLSL-LSP",
    "General",
    0,
    0,
    true,
    IsInUnifiedSettings = true,
    UnifiedSettingsCategoryMoniker = "hlslLsp.general",
    ShouldShowUnifiedSettingsPlaceholder = false)]
[Guid(PackageGuidString)]
public sealed class HlslBootstrapPackage : AsyncPackage
{
    private readonly MemoryLayoutRefreshGate memoryLayoutRefreshGate = new();
    private readonly CoalescingBackgroundRefreshCancellation memoryLayoutBackgroundRefreshCancellation =
        new(TimeSpan.FromSeconds(30));
    private long compilationInfoRequestGeneration;
    private readonly AnalysisRefreshGate compilationInfoRefreshGate = new();
    private readonly CoalescingBackgroundRefreshCancellation compilationInfoRefreshCancellation =
        new(TimeSpan.FromSeconds(30));
    private long resourceBindingsRequestGeneration;
    private readonly AnalysisRefreshGate resourceBindingsRefreshGate = new();
    private readonly CoalescingBackgroundRefreshCancellation resourceBindingsRefreshCancellation =
        new(TimeSpan.FromSeconds(30));
    private long preprocessorExplorerRequestGeneration;
    private readonly AnalysisRefreshGate preprocessorExplorerRefreshGate = new();
    private readonly CoalescingBackgroundRefreshCancellation preprocessorExplorerRefreshCancellation =
        new(TimeSpan.FromSeconds(30));
    private long entryPointDataFlowRequestGeneration;
    private readonly EntryPointDataFlowRefreshGate entryPointDataFlowRefreshGate = new();
    private readonly CoalescingBackgroundRefreshCancellation entryPointDataFlowBackgroundRefreshCancellation =
        new(TimeSpan.FromSeconds(30));
    private long computeVisualizationRequestGeneration;
    private readonly EntryPointDataFlowRefreshGate computeVisualizationRefreshGate = new();
    private readonly CoalescingBackgroundRefreshCancellation computeVisualizationBackgroundRefreshCancellation =
        new(TimeSpan.FromSeconds(30));
    private long callHierarchyRequestGeneration;
    private readonly EntryPointDataFlowRefreshGate callHierarchyRefreshGate = new();
    private readonly CoalescingBackgroundRefreshCancellation callHierarchyBackgroundRefreshCancellation =
        new(TimeSpan.FromSeconds(30));
    private readonly HashSet<string> closedAnalysisTargets =
        new(StringComparer.OrdinalIgnoreCase);
    // Resolved lazily, on the UI thread, the first time a call-hierarchy
    // root position needs to be anchored/re-resolved against a live text
    // buffer (see EnsureCallHierarchyEditorServicesAsync). Never touched by
    // any LSP-dependent code path -- these are plain VS editor services,
    // kept isolated from HlslLanguageClient/StreamJsonRpc exactly like the
    // rest of this bootstrap package.
    private IComponentModel callHierarchyComponentModel;
    private IVsEditorAdaptersFactoryService callHierarchyEditorAdapters;
    private IVsRunningDocumentTable callHierarchyRunningDocuments;
    private IVsTextManager commandTextManager;
    public const string PackageGuidString = "5ac7fbe7-1b9f-45eb-bca6-ffb9ae1ab67f";

    private static readonly object Gate = new();
    private static readonly HashSet<string> PendingDocuments =
        new(StringComparer.OrdinalIgnoreCase);
    private static HlslBootstrapPackage instance;
    private bool activationStarted;

    public static event Action OptionsChanged;

    internal static void RequestActivation(string filePath)
    {
        HlslBootstrapPackage package;
        lock (Gate)
        {
            PendingDocuments.Add(filePath);
            package = instance;
        }

        package?.JoinableTaskFactory.RunAsync(
                () => package.TryActivateLanguageClientAsync(package.DisposalToken))
            .FileAndForget("HlslLsp/TryActivate");
    }

    internal static void RunOnMainThread(Action action)
    {
        HlslBootstrapPackage package;
        lock (Gate)
        {
            package = instance;
        }
        package?.JoinableTaskFactory.RunAsync(
                async () =>
                {
                    await package.JoinableTaskFactory.SwitchToMainThreadAsync(
                        package.DisposalToken);
                    action();
                })
            .FileAndForget("HlslLsp/CommandContextMainThread");
    }

    internal static void RunInBackground(Func<Task> action, string name)
    {
        HlslBootstrapPackage package;
        lock (Gate)
        {
            package = instance;
        }
        package?.JoinableTaskFactory.RunAsync(action).FileAndForget(name);
    }

    public HlslOptionsSnapshot GetOptions()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        var page = (HlslOptionsPage)GetDialogPage(typeof(HlslOptionsPage));
        return new HlslOptionsSnapshot(
            page.FileExtensions,
            page.LanguageVersion,
            page.DxcRuntimeDirectory,
            new InlayHintOptionsSnapshot(
                page.InlayHintTypes,
                page.InlayHintParameters,
                page.InlayHintMatrixOrientation,
                page.InlayHintRegisters,
                page.InlayHintPackedOffsets,
                page.InlayHintArrayStrides,
                page.InlayHintActiveVariant));
    }

    protected override async Task InitializeAsync(
        CancellationToken cancellationToken,
        IProgress<ServiceProgressData> progress)
    {
        lock (Gate)
        {
            instance = this;
        }
        MemoryLayoutBridge.RegisterPresenter(
            (uri, line, character) =>
                JoinableTaskFactory.RunAsync(
                        () => ShowMemoryLayoutAsync(uri, line, character, DisposalToken))
                    .FileAndForget("HlslLsp/ShowMemoryLayout"));
        ComputeVisualizationBridge.RegisterPresenter(
            (uri, options) =>
                JoinableTaskFactory.RunAsync(
                        () => ShowComputeVisualizationExplicitAsync(
                            uri,
                            options,
                            DisposalToken,
                            AnalysisFreshnessCause.ConfigurationChange,
                            false))
                    .FileAndForget("HlslLsp/ShowComputeVisualization"));
        AnalysisFreshnessBridge.Register(
            kind =>
                JoinableTaskFactory.RunAsync(
                        () => RefreshTrackedAnalysisAsync(kind, DisposalToken))
                    .FileAndForget("HlslLsp/RefreshTrackedAnalysis"));
        AnalysisTrackingBridge.Register(
            (kind, mode) =>
                JoinableTaskFactory.RunAsync(
                        () => ChangeAnalysisTrackingModeAsync(
                            kind,
                            mode,
                            DisposalToken))
                    .FileAndForget("HlslLsp/ChangeAnalysisTrackingMode"));
        await RegisterCommandsAsync(cancellationToken);
        await TryActivateLanguageClientAsync(cancellationToken);
    }

    private async Task RegisterCommandsAsync(CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        var commands = await GetServiceAsync(typeof(IMenuCommandService))
            as OleMenuCommandService;
        if (commands == null)
        {
            throw new InvalidOperationException(
                "Visual Studio's command service is unavailable.");
        }

        commandTextManager =
            await GetServiceAsync(typeof(SVsTextManager)) as IVsTextManager;
        var commandSet = new Guid("cedfa85a-cd51-4825-af1f-0e05bd475426");
        var memoryLayout = new OleMenuCommand(
                (_, _) => JoinableTaskFactory.RunAsync(
                        () => ShowMemoryLayoutAsync(DisposalToken))
                    .FileAndForget("HlslLsp/ShowMemoryLayout"),
                new CommandID(commandSet, 0x0100));
        memoryLayout.BeforeQueryStatus += OnHlslContextCommandBeforeQueryStatus;
        commands.AddCommand(memoryLayout);
        var selectVariant = new OleMenuCommand(
                (_, _) => JoinableTaskFactory.RunAsync(
                        () => SelectVariantAsync(DisposalToken))
                    .FileAndForget("HlslLsp/SelectVariant"),
                new CommandID(commandSet, 0x0101));
        selectVariant.BeforeQueryStatus += OnHlslContextCommandBeforeQueryStatus;
        commands.AddCommand(selectVariant);
        var compilationInfo = new OleMenuCommand(
                (_, _) => JoinableTaskFactory.RunAsync(
                        () => ShowCompilationInfoAsync(DisposalToken))
                    .FileAndForget("HlslLsp/ShowCompilationInfo"),
                new CommandID(commandSet, 0x0102));
        compilationInfo.BeforeQueryStatus += OnHlslContextCommandBeforeQueryStatus;
        commands.AddCommand(compilationInfo);
        var resourceBindings = new OleMenuCommand(
                (_, _) => JoinableTaskFactory.RunAsync(
                        () => ShowResourceBindingsAsync(DisposalToken))
                    .FileAndForget("HlslLsp/ShowResourceBindings"),
                new CommandID(commandSet, 0x0103));
        resourceBindings.BeforeQueryStatus += OnHlslContextCommandBeforeQueryStatus;
        commands.AddCommand(resourceBindings);
        var preprocessorExplorer = new OleMenuCommand(
                (_, _) => JoinableTaskFactory.RunAsync(
                        () => ShowPreprocessorExplorerAsync(DisposalToken))
                    .FileAndForget("HlslLsp/ShowPreprocessorExplorer"),
                new CommandID(commandSet, 0x0104));
        preprocessorExplorer.BeforeQueryStatus += OnHlslContextCommandBeforeQueryStatus;
        commands.AddCommand(preprocessorExplorer);
        var entryPointDataFlow = new OleMenuCommand(
                (_, _) => JoinableTaskFactory.RunAsync(
                        () => ShowEntryPointDataFlowAsync(DisposalToken))
                    .FileAndForget("HlslLsp/ShowEntryPointDataFlow"),
                new CommandID(commandSet, 0x0105));
        entryPointDataFlow.BeforeQueryStatus += OnHlslContextCommandBeforeQueryStatus;
        commands.AddCommand(entryPointDataFlow);
        var callHierarchy = new OleMenuCommand(
                (_, _) => JoinableTaskFactory.RunAsync(
                        () => ShowCallHierarchyAsync(DisposalToken))
                    .FileAndForget("HlslLsp/ShowCallHierarchy"),
                new CommandID(commandSet, 0x0106));
        callHierarchy.BeforeQueryStatus += OnHlslContextCommandBeforeQueryStatus;
        commands.AddCommand(callHierarchy);
        var computeVisualization = new OleMenuCommand(
                (_, _) => JoinableTaskFactory.RunAsync(
                        () => ShowComputeVisualizationAsync(DisposalToken))
                    .FileAndForget("HlslLsp/ShowComputeVisualization"),
                new CommandID(commandSet, 0x0107));
        computeVisualization.BeforeQueryStatus += OnHlslContextCommandBeforeQueryStatus;
        commands.AddCommand(computeVisualization);
    }

    public void ScheduleEffectiveContextIndicatorRefresh()
        => HlslEffectiveContextIndicator.InvalidateAll();

    public void ScheduleFollowingAnalysisRefresh()
        => JoinableTaskFactory.RunAsync(
                () => RetargetFollowingAnalysisViewsAsync(DisposalToken))
            .FileAndForget("HlslLsp/RetargetFollowingAnalysisViews");

    public void ScheduleAnalysisTargetClosed(string documentPath)
    {
        if (string.IsNullOrWhiteSpace(documentPath))
        {
            return;
        }
        Uri documentUri;
        try
        {
            documentUri = new Uri(Path.GetFullPath(documentPath));
        }
        catch (Exception exception) when (
            exception is ArgumentException ||
            exception is NotSupportedException ||
            exception is PathTooLongException)
        {
            ActivityLog.LogWarning(
                nameof(HlslBootstrapPackage),
                $"Could not identify the closed analysis target '{documentPath}'.");
            return;
        }
        closedAnalysisTargets.Add(AnalysisTargetKey(documentUri));
        JoinableTaskFactory.RunAsync(
                () => MarkClosedAnalysisTargetsStaleAsync(
                    documentUri,
                    DisposalToken))
            .FileAndForget("HlslLsp/MarkClosedAnalysisTargetsStale");
    }

    public void ScheduleAnalysisTargetOpened(string documentPath)
    {
        if (TryGetAnalysisTargetKey(documentPath, out var targetKey))
        {
            closedAnalysisTargets.Remove(targetKey);
        }
    }

    public void InvalidateAnalysisViews(
        AnalysisFreshnessCause cause,
        bool refreshPending = false)
    {
        memoryLayoutRefreshGate.InvalidateForRefresh(cause, refreshPending);
        Interlocked.Increment(ref compilationInfoRequestGeneration);
        Interlocked.Increment(ref resourceBindingsRequestGeneration);
        Interlocked.Increment(ref preprocessorExplorerRequestGeneration);
        Interlocked.Increment(ref entryPointDataFlowRequestGeneration);
        Interlocked.Increment(ref computeVisualizationRequestGeneration);
        Interlocked.Increment(ref callHierarchyRequestGeneration);
        memoryLayoutBackgroundRefreshCancellation.CancelCurrent();
        compilationInfoRefreshCancellation.CancelCurrent();
        resourceBindingsRefreshCancellation.CancelCurrent();
        preprocessorExplorerRefreshCancellation.CancelCurrent();
        entryPointDataFlowBackgroundRefreshCancellation.CancelCurrent();
        computeVisualizationBackgroundRefreshCancellation.CancelCurrent();
        callHierarchyBackgroundRefreshCancellation.CancelCurrent();
        if (refreshPending)
        {
            entryPointDataFlowRefreshGate.RecordRefreshNeeded(cause);
            computeVisualizationRefreshGate.RecordRefreshNeeded(cause);
            callHierarchyRefreshGate.RecordRefreshNeeded(cause);
            compilationInfoRefreshGate.RecordRefreshNeeded(cause);
            resourceBindingsRefreshGate.RecordRefreshNeeded(cause);
            preprocessorExplorerRefreshGate.RecordRefreshNeeded(cause);
        }
        JoinableTaskFactory.RunAsync(
                () => MarkOpenAnalysisViewsStaleAsync(
                    cause,
                    refreshPending,
                    DisposalToken))
            .FileAndForget("HlslLsp/MarkAnalysisViewsStale");
    }

    private async Task MarkOpenAnalysisViewsStaleAsync(
        AnalysisFreshnessCause cause,
        bool refreshPending,
        CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        foreach (var windowType in AnalysisWindowTypes)
        {
            if (await FindToolWindowAsync(
                        windowType,
                        0,
                        false,
                        cancellationToken)
                    is IAnalysisFreshnessView view)
            {
                view.MarkStale(cause, refreshPending);
            }
        }
    }

    private async Task BeginAnalysisRefreshIfOpenAsync(
        Type windowType,
        AnalysisFreshnessCause cause,
        CancellationToken cancellationToken,
        bool pinTracking = true)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (await FindToolWindowAsync(
                    windowType,
                    0,
                    false,
                    cancellationToken)
                is IAnalysisFreshnessView view)
        {
            if (pinTracking && view is IAnalysisTrackingView trackingView)
            {
                InvalidateAnalysisTrackingRequest(AnalysisWindowKind(windowType));
                trackingView.CancelTrackingRefresh();
                trackingView.SetTrackingMode(AnalysisTrackingPolicy.DefaultMode);
            }
            view.BeginRefresh(cause);
        }
    }

    private async Task RefreshTrackedAnalysisAsync(
        AnalysisViewKind kind,
        CancellationToken cancellationToken)
    {
        switch (kind)
        {
            case AnalysisViewKind.MemoryLayout:
                await RefreshMemoryLayoutIfOpenAsync(
                    null,
                    cancellationToken,
                    AnalysisFreshnessCause.ManualRefresh);
                break;
            case AnalysisViewKind.CompilationInfo:
                await RefreshCompilationInfoIfOpenAsync(
                    null,
                    cancellationToken,
                    AnalysisFreshnessCause.ManualRefresh);
                break;
            case AnalysisViewKind.ResourceBindings:
                await RefreshResourceBindingsIfOpenAsync(
                    null,
                    cancellationToken,
                    AnalysisFreshnessCause.ManualRefresh);
                break;
            case AnalysisViewKind.PreprocessorExplorer:
                await RefreshPreprocessorExplorerIfOpenAsync(
                    null,
                    cancellationToken,
                    AnalysisFreshnessCause.ManualRefresh);
                break;
            case AnalysisViewKind.EntryPointDataFlow:
                await RefreshEntryPointDataFlowIfOpenAsync(
                    null,
                    cancellationToken,
                    AnalysisFreshnessCause.ManualRefresh);
                break;
            case AnalysisViewKind.ComputeVisualization:
                await RefreshComputeVisualizationIfOpenAsync(
                    null,
                    cancellationToken,
                    AnalysisFreshnessCause.ManualRefresh);
                break;
            case AnalysisViewKind.CallHierarchy:
                await RefreshCallHierarchyIfOpenAsync(
                    null,
                    cancellationToken,
                    AnalysisFreshnessCause.ManualRefresh);
                break;
            default:
                throw new ArgumentOutOfRangeException(nameof(kind));
        }
    }

    private async Task ChangeAnalysisTrackingModeAsync(
        AnalysisViewKind kind,
        AnalysisTrackingMode mode,
        CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        var view = await FindOpenAnalysisTrackingViewAsync(kind, cancellationToken);
        if (view == null)
        {
            return;
        }

        view.SetTrackingMode(mode);
        if (mode == AnalysisTrackingMode.Pinned)
        {
            InvalidateAnalysisTrackingRequest(kind);
            view.CancelTrackingRefresh();
            return;
        }

        if (!TryGetActiveHlslEditorContext(
                out var uri,
                out var line,
                out var character,
                out var lines))
        {
            InvalidateAnalysisTrackingRequest(kind);
            view.MarkStale(AnalysisFreshnessCause.ActiveShaderUnavailable);
            return;
        }
        await RetargetAnalysisViewAsync(
            kind,
            view,
            uri,
            line,
            character,
            lines,
            cancellationToken);
    }

    private async Task RetargetFollowingAnalysisViewsAsync(
        CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        var activeShaderAvailable = TryGetActiveHlslEditorContext(
            out var uri,
            out var line,
            out var character,
            out var lines);
        var retargets = new List<Task>();
        foreach (AnalysisViewKind kind in Enum.GetValues(typeof(AnalysisViewKind)))
        {
            var view = await FindOpenAnalysisTrackingViewAsync(kind, cancellationToken);
            if (view == null)
            {
                continue;
            }
            var action = AnalysisTrackingPolicy.OnActiveViewChanged(
                view.TrackingMode,
                activeShaderAvailable);
            if (action == AnalysisTrackingAction.None)
            {
                continue;
            }
            if (action == AnalysisTrackingAction.MarkUnavailable)
            {
                InvalidateAnalysisTrackingRequest(kind);
                view.MarkStale(AnalysisFreshnessCause.ActiveShaderUnavailable);
                continue;
            }
            retargets.Add(
                RetargetAnalysisViewAsync(
                    kind,
                    view,
                    uri,
                    line,
                    character,
                    lines,
                    cancellationToken));
        }
        await Task.WhenAll(retargets);
    }

    private async Task<IAnalysisTrackingView> FindOpenAnalysisTrackingViewAsync(
        AnalysisViewKind kind,
        CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        return await FindToolWindowAsync(
                AnalysisWindowType(kind),
                0,
                false,
                cancellationToken)
            as IAnalysisTrackingView;
    }

    private async Task MarkClosedAnalysisTargetsStaleAsync(
        Uri documentUri,
        CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        var targetKey = AnalysisTargetKey(documentUri);
        if (!closedAnalysisTargets.Contains(targetKey))
        {
            return;
        }
        foreach (AnalysisViewKind kind in Enum.GetValues(typeof(AnalysisViewKind)))
        {
            var view = await FindOpenAnalysisTrackingViewAsync(kind, cancellationToken);
            if (!closedAnalysisTargets.Contains(targetKey))
            {
                return;
            }
            if (view == null ||
                !AnalysisTrackingPolicy.IsSameDocument(
                    view.TrackingDocumentUri,
                    documentUri))
            {
                continue;
            }
            InvalidateAnalysisTrackingRequest(kind);
            view.MarkStale(AnalysisFreshnessCause.TrackedShaderClosed);
        }
    }

    private bool IsAnalysisTargetClosed(Uri documentUri)
        => documentUri != null &&
           closedAnalysisTargets.Contains(AnalysisTargetKey(documentUri));

    private static string AnalysisTargetKey(Uri documentUri)
        => documentUri.IsFile
            ? Path.GetFullPath(documentUri.LocalPath)
            : documentUri.AbsoluteUri;

    private static bool TryGetAnalysisTargetKey(
        string documentPath,
        out string targetKey)
    {
        targetKey = null;
        if (string.IsNullOrWhiteSpace(documentPath))
        {
            return false;
        }
        try
        {
            targetKey = Path.GetFullPath(documentPath);
            return true;
        }
        catch (Exception exception) when (
            exception is ArgumentException ||
            exception is NotSupportedException ||
            exception is PathTooLongException)
        {
            return false;
        }
    }

    private static Type AnalysisWindowType(AnalysisViewKind kind)
        => kind switch
        {
            AnalysisViewKind.MemoryLayout => typeof(MemoryLayoutToolWindow),
            AnalysisViewKind.CompilationInfo => typeof(CompilationInfoToolWindow),
            AnalysisViewKind.ResourceBindings => typeof(ResourceBindingsToolWindow),
            AnalysisViewKind.PreprocessorExplorer => typeof(PreprocessorExplorerToolWindow),
            AnalysisViewKind.EntryPointDataFlow => typeof(EntryPointDataFlowToolWindow),
            AnalysisViewKind.ComputeVisualization => typeof(ComputeVisualizationToolWindow),
            AnalysisViewKind.CallHierarchy => typeof(CallHierarchyExplorerToolWindow),
            _ => throw new ArgumentOutOfRangeException(nameof(kind)),
        };

    private static AnalysisViewKind AnalysisWindowKind(Type windowType)
    {
        if (windowType == typeof(MemoryLayoutToolWindow))
        {
            return AnalysisViewKind.MemoryLayout;
        }
        if (windowType == typeof(CompilationInfoToolWindow))
        {
            return AnalysisViewKind.CompilationInfo;
        }
        if (windowType == typeof(ResourceBindingsToolWindow))
        {
            return AnalysisViewKind.ResourceBindings;
        }
        if (windowType == typeof(PreprocessorExplorerToolWindow))
        {
            return AnalysisViewKind.PreprocessorExplorer;
        }
        if (windowType == typeof(EntryPointDataFlowToolWindow))
        {
            return AnalysisViewKind.EntryPointDataFlow;
        }
        if (windowType == typeof(ComputeVisualizationToolWindow))
        {
            return AnalysisViewKind.ComputeVisualization;
        }
        if (windowType == typeof(CallHierarchyExplorerToolWindow))
        {
            return AnalysisViewKind.CallHierarchy;
        }
        throw new ArgumentOutOfRangeException(nameof(windowType));
    }

    private async Task RetargetAnalysisViewAsync(
        AnalysisViewKind kind,
        IAnalysisTrackingView view,
        Uri uri,
        int line,
        int character,
        IVsTextLines lines,
        CancellationToken cancellationToken)
    {
        if (view.TrackingMode != AnalysisTrackingMode.FollowActiveShader)
        {
            return;
        }
        switch (kind)
        {
            case AnalysisViewKind.MemoryLayout:
                await RetargetMemoryLayoutAsync(
                    (MemoryLayoutToolWindow)view,
                    uri,
                    line,
                    character,
                    lines,
                    cancellationToken);
                break;
            case AnalysisViewKind.CompilationInfo:
                await RetargetCompilationInfoAsync(
                    (CompilationInfoToolWindow)view,
                    uri,
                    cancellationToken);
                break;
            case AnalysisViewKind.ResourceBindings:
                await RetargetResourceBindingsAsync(
                    (ResourceBindingsToolWindow)view,
                    uri,
                    cancellationToken);
                break;
            case AnalysisViewKind.PreprocessorExplorer:
                await RetargetPreprocessorExplorerAsync(
                    (PreprocessorExplorerToolWindow)view,
                    uri,
                    cancellationToken);
                break;
            case AnalysisViewKind.EntryPointDataFlow:
                await RetargetEntryPointDataFlowAsync(
                    (EntryPointDataFlowToolWindow)view,
                    uri,
                    cancellationToken);
                break;
            case AnalysisViewKind.ComputeVisualization:
                await RetargetComputeVisualizationAsync(
                    (ComputeVisualizationToolWindow)view,
                    uri,
                    cancellationToken);
                break;
            case AnalysisViewKind.CallHierarchy:
                await RetargetCallHierarchyAsync(
                    (CallHierarchyExplorerToolWindow)view,
                    uri,
                    line,
                    character,
                    lines,
                    cancellationToken);
                break;
            default:
                throw new ArgumentOutOfRangeException(nameof(kind));
        }
    }

    private void InvalidateAnalysisTrackingRequest(AnalysisViewKind kind)
    {
        switch (kind)
        {
            case AnalysisViewKind.MemoryLayout:
                memoryLayoutRefreshGate.InvalidateForRefresh(
                    AnalysisFreshnessCause.ActiveShaderUnavailable,
                    false);
                memoryLayoutBackgroundRefreshCancellation.CancelCurrent();
                break;
            case AnalysisViewKind.CompilationInfo:
                Interlocked.Increment(ref compilationInfoRequestGeneration);
                compilationInfoRefreshCancellation.CancelCurrent();
                break;
            case AnalysisViewKind.ResourceBindings:
                Interlocked.Increment(ref resourceBindingsRequestGeneration);
                resourceBindingsRefreshCancellation.CancelCurrent();
                break;
            case AnalysisViewKind.PreprocessorExplorer:
                Interlocked.Increment(ref preprocessorExplorerRequestGeneration);
                preprocessorExplorerRefreshCancellation.CancelCurrent();
                break;
            case AnalysisViewKind.EntryPointDataFlow:
                Interlocked.Increment(ref entryPointDataFlowRequestGeneration);
                entryPointDataFlowBackgroundRefreshCancellation.CancelCurrent();
                break;
            case AnalysisViewKind.ComputeVisualization:
                Interlocked.Increment(ref computeVisualizationRequestGeneration);
                computeVisualizationBackgroundRefreshCancellation.CancelCurrent();
                break;
            case AnalysisViewKind.CallHierarchy:
                Interlocked.Increment(ref callHierarchyRequestGeneration);
                callHierarchyBackgroundRefreshCancellation.CancelCurrent();
                break;
            default:
                throw new ArgumentOutOfRangeException(nameof(kind));
        }
    }

    private static readonly Type[] AnalysisWindowTypes =
    {
        typeof(MemoryLayoutToolWindow),
        typeof(CompilationInfoToolWindow),
        typeof(ResourceBindingsToolWindow),
        typeof(PreprocessorExplorerToolWindow),
        typeof(EntryPointDataFlowToolWindow),
        typeof(ComputeVisualizationToolWindow),
        typeof(CallHierarchyExplorerToolWindow),
    };

    private async Task RetargetMemoryLayoutAsync(
        MemoryLayoutToolWindow window,
        Uri uri,
        int line,
        int character,
        IVsTextLines lines,
        CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (window.TrackingMode != AnalysisTrackingMode.FollowActiveShader)
        {
            return;
        }
        await EnsureCallHierarchyEditorServicesAsync(cancellationToken);
        if (window.TrackingMode != AnalysisTrackingMode.FollowActiveShader)
        {
            return;
        }
        var buffer = lines is IVsTextBuffer bufferAdapter
            ? callHierarchyEditorAdapters?.GetDocumentBuffer(bufferAdapter)
            : null;
        var trackingPoint = buffer == null
            ? null
            : CreateRootTrackingPoint(buffer, line, character);
        var generation = memoryLayoutRefreshGate.EnterExplicitRequest();
        window.BeginRetarget(uri, line, character, trackingPoint);
        window.BeginRefresh(AnalysisFreshnessCause.ActiveShaderChange);
        try
        {
            var requestCancellation =
                memoryLayoutBackgroundRefreshCancellation.BeginNext(cancellationToken);
            await RequestMemoryLayoutAsync(
                uri,
                line,
                character,
                trackingPoint,
                generation,
                requestCancellation.Token,
                cancellationToken,
                window);
        }
        finally
        {
            if (memoryLayoutRefreshGate.ExitExplicitRequest(out var pendingCause))
            {
                await RefreshMemoryLayoutIfOpenAsync(
                    null,
                    cancellationToken,
                    pendingCause);
            }
        }
    }

    private async Task RetargetCompilationInfoAsync(
        CompilationInfoToolWindow window,
        Uri uri,
        CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (window.TrackingMode != AnalysisTrackingMode.FollowActiveShader)
        {
            return;
        }
        compilationInfoRefreshGate.EnterExplicitRequest();
        window.BeginRetarget(uri);
        window.BeginRefresh(AnalysisFreshnessCause.ActiveShaderChange);
        try
        {
            var requestCancellation =
                compilationInfoRefreshCancellation.BeginNext(cancellationToken);
            await ShowCompilationInfoAsync(
                uri,
                requestCancellation.Token,
                window,
                cancellationToken);
        }
        finally
        {
            if (compilationInfoRefreshGate.ExitExplicitRequest(out var pendingCause))
            {
                await RefreshCompilationInfoIfOpenAsync(
                    null,
                    cancellationToken,
                    pendingCause);
            }
        }
    }

    private async Task RetargetResourceBindingsAsync(
        ResourceBindingsToolWindow window,
        Uri uri,
        CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (window.TrackingMode != AnalysisTrackingMode.FollowActiveShader)
        {
            return;
        }
        resourceBindingsRefreshGate.EnterExplicitRequest();
        window.BeginRetarget(uri);
        window.BeginRefresh(AnalysisFreshnessCause.ActiveShaderChange);
        try
        {
            var requestCancellation =
                resourceBindingsRefreshCancellation.BeginNext(cancellationToken);
            await ShowResourceBindingsAsync(
                uri,
                requestCancellation.Token,
                window,
                cancellationToken);
        }
        finally
        {
            if (resourceBindingsRefreshGate.ExitExplicitRequest(out var pendingCause))
            {
                await RefreshResourceBindingsIfOpenAsync(
                    null,
                    cancellationToken,
                    pendingCause);
            }
        }
    }

    private async Task RetargetPreprocessorExplorerAsync(
        PreprocessorExplorerToolWindow window,
        Uri uri,
        CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (window.TrackingMode != AnalysisTrackingMode.FollowActiveShader)
        {
            return;
        }
        preprocessorExplorerRefreshGate.EnterExplicitRequest();
        window.BeginRetarget(uri);
        window.BeginRefresh(AnalysisFreshnessCause.ActiveShaderChange);
        try
        {
            var requestCancellation =
                preprocessorExplorerRefreshCancellation.BeginNext(cancellationToken);
            await ShowPreprocessorExplorerAsync(
                uri,
                requestCancellation.Token,
                window,
                cancellationToken);
        }
        finally
        {
            if (preprocessorExplorerRefreshGate.ExitExplicitRequest(out var pendingCause))
            {
                await RefreshPreprocessorExplorerIfOpenAsync(
                    null,
                    cancellationToken,
                    pendingCause);
            }
        }
    }

    private async Task RetargetEntryPointDataFlowAsync(
        EntryPointDataFlowToolWindow window,
        Uri uri,
        CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (window.TrackingMode != AnalysisTrackingMode.FollowActiveShader)
        {
            return;
        }
        entryPointDataFlowRefreshGate.EnterExplicitRequest();
        window.BeginRetarget(uri);
        window.BeginRefresh(AnalysisFreshnessCause.ActiveShaderChange);
        try
        {
            var requestCancellation =
                entryPointDataFlowBackgroundRefreshCancellation.BeginNext(cancellationToken);
            await ShowEntryPointDataFlowAsync(
                uri,
                requestCancellation.Token,
                window,
                cancellationToken);
        }
        finally
        {
            if (entryPointDataFlowRefreshGate.ExitExplicitRequest(out var pendingCause))
            {
                await RefreshEntryPointDataFlowIfOpenAsync(
                    null,
                    cancellationToken,
                    pendingCause);
            }
        }
    }

    private async Task RetargetComputeVisualizationAsync(
        ComputeVisualizationToolWindow window,
        Uri uri,
        CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (window.TrackingMode != AnalysisTrackingMode.FollowActiveShader)
        {
            return;
        }
        computeVisualizationRefreshGate.EnterExplicitRequest();
        window.TrackRequest(uri);
        window.BeginRefresh(AnalysisFreshnessCause.ActiveShaderChange);
        try
        {
            var requestCancellation =
                computeVisualizationBackgroundRefreshCancellation.BeginNext(cancellationToken);
            await ShowComputeVisualizationAsync(
                uri,
                ComputeVisualizationRefreshLogic.OptionsForBackgroundRefresh(
                    window.SubmittedOptions),
                requestCancellation.Token,
                window,
                cancellationToken);
        }
        finally
        {
            if (computeVisualizationRefreshGate.ExitExplicitRequest(out var pendingCause))
            {
                await RefreshComputeVisualizationIfOpenAsync(
                    null,
                    cancellationToken,
                    pendingCause);
            }
        }
    }

    private async Task RetargetCallHierarchyAsync(
        CallHierarchyExplorerToolWindow window,
        Uri uri,
        int line,
        int character,
        IVsTextLines lines,
        CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (window.TrackingMode != AnalysisTrackingMode.FollowActiveShader)
        {
            return;
        }
        await EnsureCallHierarchyEditorServicesAsync(cancellationToken);
        if (window.TrackingMode != AnalysisTrackingMode.FollowActiveShader)
        {
            return;
        }
        var buffer = lines is IVsTextBuffer bufferAdapter
            ? callHierarchyEditorAdapters?.GetDocumentBuffer(bufferAdapter)
            : null;
        var trackingPoint = buffer == null
            ? null
            : CreateRootTrackingPoint(buffer, line, character);
        callHierarchyRefreshGate.EnterExplicitRequest();
        window.BeginRetarget(uri, line, character, trackingPoint);
        window.BeginRefresh(AnalysisFreshnessCause.ActiveShaderChange);
        try
        {
            var requestCancellation =
                callHierarchyBackgroundRefreshCancellation.BeginNext(cancellationToken);
            await EstablishCallHierarchyRootAsync(
                uri,
                line,
                character,
                trackingPoint,
                requestCancellation.Token,
                cancellationToken,
                window);
        }
        finally
        {
            if (callHierarchyRefreshGate.ExitExplicitRequest(out var pendingCause))
            {
                await RefreshCallHierarchyIfOpenAsync(
                    null,
                    cancellationToken,
                    pendingCause);
            }
        }
    }

    private async Task RetryPendingCallHierarchyRetargetAsync(
        CallHierarchyExplorerToolWindow window,
        CancellationToken cancellationToken)
    {
        callHierarchyRefreshGate.EnterExplicitRequest();
        window.BeginRefresh(AnalysisFreshnessCause.ActiveShaderChange);
        try
        {
            var requestCancellation =
                callHierarchyBackgroundRefreshCancellation.BeginNext(cancellationToken);
            await EstablishCallHierarchyRootAsync(
                window.RootDocumentUri,
                window.RootLine,
                window.RootCharacter,
                window.RootTrackingPoint,
                requestCancellation.Token,
                cancellationToken,
                window);
        }
        finally
        {
            if (callHierarchyRefreshGate.ExitExplicitRequest(out var pendingCause))
            {
                await RefreshCallHierarchyIfOpenAsync(
                    null,
                    cancellationToken,
                    pendingCause);
            }
        }
    }

    private async Task ShowMemoryLayoutAsync(CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (!TryGetActiveHlslEditorContext(
                out var uri,
                out var line,
                out var character,
                out var lines))
        {
            return;
        }

        await EnsureCallHierarchyEditorServicesAsync(cancellationToken);
        var buffer = lines is IVsTextBuffer bufferAdapter
            ? callHierarchyEditorAdapters?.GetDocumentBuffer(bufferAdapter)
            : null;
        await ShowMemoryLayoutExplicitAsync(
            uri,
            line,
            character,
            buffer == null ? null : CreateRootTrackingPoint(buffer, line, character),
            cancellationToken);
    }

    private async Task ShowMemoryLayoutAsync(
        Uri uri,
        int line,
        int character,
        CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        await EnsureCallHierarchyEditorServicesAsync(cancellationToken);
        var buffer = TryGetOpenCallHierarchyBuffer(uri);
        await ShowMemoryLayoutExplicitAsync(
            uri,
            line,
            character,
            buffer == null ? null : CreateRootTrackingPoint(buffer, line, character),
            cancellationToken);
    }

    private async Task ShowMemoryLayoutExplicitAsync(
        Uri uri,
        int line,
        int character,
        ITrackingPoint trackingPoint,
        CancellationToken cancellationToken)
    {
        await BeginAnalysisRefreshIfOpenAsync(
            typeof(MemoryLayoutToolWindow),
            AnalysisFreshnessCause.ManualRefresh,
            cancellationToken);
        var generation = memoryLayoutRefreshGate.EnterExplicitRequest();
        try
        {
            var requestCancellation =
                memoryLayoutBackgroundRefreshCancellation.BeginNext(cancellationToken);
            await RequestMemoryLayoutAsync(
                uri,
                line,
                character,
                trackingPoint,
                generation,
                requestCancellation.Token,
                cancellationToken,
                null);
        }
        finally
        {
            if (memoryLayoutRefreshGate.ExitExplicitRequest(
                    out var pendingCause))
            {
                await RefreshMemoryLayoutIfOpenAsync(
                    null,
                    cancellationToken,
                    pendingCause);
            }
        }
    }

    private async Task RequestMemoryLayoutAsync(
        Uri uri,
        int line,
        int character,
        ITrackingPoint trackingPoint,
        long generation,
        CancellationToken cancellationToken,
        CancellationToken ambientCancellationToken,
        MemoryLayoutToolWindow existingWindow)
    {
        MemoryLayoutModel layout = null;
        string failureMessage = null;
        try
        {
            layout = await MemoryLayoutBridge.RequestAsync(
                uri,
                line,
                character,
                cancellationToken);
        }
        catch (OperationCanceledException) when (ambientCancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException)
        {
            failureMessage = "The memory layout request was cancelled.";
        }
        catch (Exception error)
        {
            failureMessage = "Could not retrieve memory layout information: " + error.Message;
        }
        if (!memoryLayoutRefreshGate.IsCurrent(generation))
        {
            return;
        }
        await JoinableTaskFactory.SwitchToMainThreadAsync(ambientCancellationToken);
        var window = existingWindow;
        if (window == null)
        {
            window = await ShowToolWindowAsync(
                typeof(MemoryLayoutToolWindow),
                0,
                true,
                ambientCancellationToken) as MemoryLayoutToolWindow;
        }
        if (memoryLayoutRefreshGate.IsCurrent(generation))
        {
            if (failureMessage != null)
            {
                window?.SetError(
                    uri,
                    line,
                    character,
                    trackingPoint,
                    failureMessage,
                    true);
            }
            else if (layout == null)
            {
                window?.SetError(
                    uri,
                    line,
                    character,
                    trackingPoint,
                    "No compiler-authoritative memory layout is available at this position.",
                    true);
            }
            else
            {
                window?.SetLayout(uri, line, character, trackingPoint, layout);
            }
        }
    }

    internal static void ExecuteSelectVariantCommand()
    {
        HlslBootstrapPackage package;
        lock (Gate)
        {
            package = instance;
        }
        package?.JoinableTaskFactory.RunAsync(
                async () =>
                {
                    await package.JoinableTaskFactory.SwitchToMainThreadAsync(
                        package.DisposalToken);
                    var shell = await package.GetServiceAsync(typeof(SVsUIShell)) as IVsUIShell;
                    if (shell == null)
                    {
                        return;
                    }
                    var commandSet = new Guid("cedfa85a-cd51-4825-af1f-0e05bd475426");
                    ErrorHandler.ThrowOnFailure(
                        shell.PostExecCommand(
                            ref commandSet,
                            0x0101,
                            0,
                            null));
                })
            .FileAndForget("HlslLsp/SelectVariantFromIndicator");
    }

    public async Task RefreshMemoryLayoutIfOpenAsync(
        string savedFilePath,
        CancellationToken cancellationToken,
        AnalysisFreshnessCause cause = AnalysisFreshnessCause.Unknown)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (await FindToolWindowAsync(
                    typeof(MemoryLayoutToolWindow),
                    0,
                    false,
                    cancellationToken)
                is not MemoryLayoutToolWindow window ||
            window.DocumentUri == null)
        {
            return;
        }
        if (!AnalysisSaveRefreshPolicy.ShouldRefresh(
                AnalysisViewKind.MemoryLayout,
                window.DocumentUri,
                savedFilePath,
                ParseExtensions(GetOptions().FileExtensions)))
        {
            return;
        }
        if (IsAnalysisTargetClosed(window.DocumentUri))
        {
            return;
        }
        if (!memoryLayoutRefreshGate.TryBeginBackgroundRefresh(
                cause,
                out var generation))
        {
            memoryLayoutRefreshGate.InvalidateForRefresh(cause, true);
            window.MarkStale(cause, true);
            return;
        }
        if (!memoryLayoutRefreshGate.IsCurrent(generation))
        {
            return;
        }
        await EnsureCallHierarchyEditorServicesAsync(cancellationToken);
        if (!memoryLayoutRefreshGate.IsCurrent(generation))
        {
            return;
        }
        var liveBuffer = TryGetOpenCallHierarchyBuffer(window.DocumentUri);
        var trackingPoint = window.TrackingPoint;
        if (trackingPoint != null &&
            !MemoryLayoutTrackingBuffer.IsCurrent(liveBuffer, trackingPoint.TextBuffer))
        {
            window.SetError(
                window.DocumentUri,
                window.Line,
                window.Character,
                null,
                "The tracked document buffer is stale or no longer open. Select Memory Layout again at the desired position.",
                true);
            return;
        }
        var trackedPosition = trackingPoint == null
            ? ((int Line, int Character)?)(window.Line, window.Character)
            : ResolveTrackedPosition(trackingPoint);
        if (!trackedPosition.HasValue)
        {
            window.SetError(
                window.DocumentUri,
                window.Line,
                window.Character,
                null,
                "The tracked declaration is no longer available. Select Memory Layout again at the desired position.",
                true);
            return;
        }
        window.UpdateTrackedPosition(
            trackedPosition.Value.Line,
            trackedPosition.Value.Character);
        window.BeginRefresh(cause);
        var refreshCancellation =
            memoryLayoutBackgroundRefreshCancellation.BeginNext(cancellationToken);
        await RequestMemoryLayoutAsync(
            window.DocumentUri,
            trackedPosition.Value.Line,
            trackedPosition.Value.Character,
            trackingPoint,
            generation,
            refreshCancellation.Token,
            cancellationToken,
            window);
    }

    private async Task ShowCompilationInfoAsync(CancellationToken cancellationToken)
    {
        var uri = await GetActiveDocumentUriAsync(cancellationToken);
        if (uri == null)
        {
            await ShowInformationAsync(
                "Open an HLSL document, then choose HLSL > Shader Compilation.",
                cancellationToken);
            return;
        }
        await BeginAnalysisRefreshIfOpenAsync(
            typeof(CompilationInfoToolWindow),
            AnalysisFreshnessCause.ManualRefresh,
            cancellationToken);
        compilationInfoRefreshGate.EnterExplicitRequest();
        try
        {
            var requestCancellation =
                compilationInfoRefreshCancellation.BeginNext(cancellationToken);
            await ShowCompilationInfoAsync(
                uri,
                requestCancellation.Token,
                null,
                cancellationToken);
        }
        finally
        {
            if (compilationInfoRefreshGate.ExitExplicitRequest(
                    out var pendingCause))
            {
                await RefreshCompilationInfoIfOpenAsync(
                    null,
                    cancellationToken,
                    pendingCause);
            }
        }
    }

    // The generation guard mirrors ShowMemoryLayoutAsync: a stale response
    // (e.g. from a superseded variant change or an earlier command
    // invocation) can never overwrite a newer one. A failed or cancelled
    // request never regresses the window to the "open a document"
    // placeholder or leaves it stuck: it keeps the last successful content
    // when one exists, and otherwise shows an explicit error.
    private async Task ShowCompilationInfoAsync(
        Uri uri,
        CancellationToken cancellationToken,
        CompilationInfoToolWindow existingWindow = null,
        CancellationToken ambientCancellationToken = default)
    {
        var generation = Interlocked.Increment(ref compilationInfoRequestGeneration);
        CompilationInfoModel info = null;
        string failureMessage = null;
        try
        {
            info = await CompilationInfoBridge.RequestAsync(uri, cancellationToken);
        }
        catch (OperationCanceledException) when (ambientCancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException)
        {
            failureMessage = "The shader compilation request was cancelled.";
        }
        catch (Exception error)
        {
            failureMessage =
                "Could not retrieve shader compilation information: " + error.Message;
        }
        if (generation != Interlocked.Read(ref compilationInfoRequestGeneration))
        {
            return;
        }
        // The request token may represent the bounded RPC timeout. Once a
        // result or failure message is ready, use only the ambient package
        // token for presentation so a timeout can still be shown to the user.
        await JoinableTaskFactory.SwitchToMainThreadAsync(ambientCancellationToken);
        var window = existingWindow;
        if (window == null)
        {
            window = await ShowToolWindowAsync(
                typeof(CompilationInfoToolWindow),
                0,
                true,
                ambientCancellationToken) as CompilationInfoToolWindow;
        }
        if (generation != Interlocked.Read(ref compilationInfoRequestGeneration))
        {
            return;
        }
        if (failureMessage != null)
        {
            window?.SetError(uri, failureMessage, existingWindow != null);
            return;
        }
        if (info == null)
        {
            window?.SetError(
                uri,
                "The HLSL language server is not ready to provide shader compilation information.",
                existingWindow != null);
            return;
        }
        window?.SetInfo(uri, info);
    }

    // Invoked after an active-variant selection or a document save. Only
    // refreshes an already-open window, and only for a save whose saved file
    // matches the window's tracked document, so this cannot start a request
    // storm from unrelated documents or from opening the window for the
    // first time.
    public async Task RefreshCompilationInfoIfOpenAsync(
        string savedFilePath,
        CancellationToken cancellationToken,
        AnalysisFreshnessCause cause = AnalysisFreshnessCause.Unknown)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (await FindToolWindowAsync(
                    typeof(CompilationInfoToolWindow),
                    0,
                    false,
                    cancellationToken)
                is not CompilationInfoToolWindow window ||
            window.DocumentUri == null)
        {
            return;
        }
        if (!AnalysisSaveRefreshPolicy.ShouldRefresh(
                AnalysisViewKind.CompilationInfo,
                window.DocumentUri,
                savedFilePath,
                ParseExtensions(GetOptions().FileExtensions)))
        {
            return;
        }
        if (IsAnalysisTargetClosed(window.DocumentUri))
        {
            return;
        }
        if (!compilationInfoRefreshGate.TryBeginBackgroundRefresh(cause))
        {
            Interlocked.Increment(ref compilationInfoRequestGeneration);
            window.MarkStale(cause, true);
            return;
        }
        window.BeginRefresh(cause);
        var requestCancellation =
            compilationInfoRefreshCancellation.BeginNext(cancellationToken);
        await ShowCompilationInfoAsync(
            window.DocumentUri,
            requestCancellation.Token,
            window,
            cancellationToken);
    }

    private async Task ShowResourceBindingsAsync(CancellationToken cancellationToken)
    {
        var uri = await GetActiveDocumentUriAsync(cancellationToken);
        if (uri == null)
        {
            await ShowInformationAsync(
                "Open an HLSL document, then choose HLSL > Resource Bindings.",
                cancellationToken);
            return;
        }
        await BeginAnalysisRefreshIfOpenAsync(
            typeof(ResourceBindingsToolWindow),
            AnalysisFreshnessCause.ManualRefresh,
            cancellationToken);
        resourceBindingsRefreshGate.EnterExplicitRequest();
        try
        {
            var requestCancellation =
                resourceBindingsRefreshCancellation.BeginNext(cancellationToken);
            await ShowResourceBindingsAsync(
                uri,
                requestCancellation.Token,
                null,
                cancellationToken);
        }
        finally
        {
            if (resourceBindingsRefreshGate.ExitExplicitRequest(
                    out var pendingCause))
            {
                await RefreshResourceBindingsIfOpenAsync(
                    null,
                    cancellationToken,
                    pendingCause);
            }
        }
    }

    // Mirrors ShowCompilationInfoAsync: the generation guard ensures a stale
    // response (e.g. from a superseded variant change or an earlier command
    // invocation) can never overwrite a newer one. A failed or cancelled
    // request never regresses the window to the "open a document"
    // placeholder or leaves it stuck: it keeps the last successful content
    // when one exists, and otherwise shows an explicit error. Reuses the
    // same CompilationInfoBridge request as the Shader Compilation window;
    // no new RPC surface is introduced for Resource Bindings.
    private async Task ShowResourceBindingsAsync(
        Uri uri,
        CancellationToken cancellationToken,
        ResourceBindingsToolWindow existingWindow = null,
        CancellationToken ambientCancellationToken = default)
    {
        var generation = Interlocked.Increment(ref resourceBindingsRequestGeneration);
        CompilationInfoModel info = null;
        string failureMessage = null;
        try
        {
            info = await CompilationInfoBridge.RequestAsync(uri, cancellationToken);
        }
        catch (OperationCanceledException) when (ambientCancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException)
        {
            failureMessage = "The resource bindings request was cancelled.";
        }
        catch (Exception error)
        {
            failureMessage =
                "Could not retrieve resource binding information: " + error.Message;
        }
        if (generation != Interlocked.Read(ref resourceBindingsRequestGeneration))
        {
            return;
        }
        // The request token may represent the bounded RPC timeout. Once a
        // result or failure message is ready, use only the ambient package
        // token for presentation so a timeout can still be shown to the user.
        await JoinableTaskFactory.SwitchToMainThreadAsync(ambientCancellationToken);
        var window = existingWindow;
        if (window == null)
        {
            window = await ShowToolWindowAsync(
                typeof(ResourceBindingsToolWindow),
                0,
                true,
                ambientCancellationToken) as ResourceBindingsToolWindow;
        }
        if (generation != Interlocked.Read(ref resourceBindingsRequestGeneration))
        {
            return;
        }
        if (failureMessage != null)
        {
            window?.SetError(uri, failureMessage, existingWindow != null);
            return;
        }
        if (info == null)
        {
            window?.SetError(
                uri,
                "The HLSL language server is not ready to provide resource binding information.",
                existingWindow != null);
            return;
        }
        window?.SetInfo(uri, info);
    }

    // Invoked after an active-variant selection or a document save. Only
    // refreshes an already-open window, and only for a save whose saved file
    // matches the window's tracked document, so this cannot start a request
    // storm from unrelated documents or from opening the window for the
    // first time.
    public async Task RefreshResourceBindingsIfOpenAsync(
        string savedFilePath,
        CancellationToken cancellationToken,
        AnalysisFreshnessCause cause = AnalysisFreshnessCause.Unknown)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (await FindToolWindowAsync(
                    typeof(ResourceBindingsToolWindow),
                    0,
                    false,
                    cancellationToken)
                is not ResourceBindingsToolWindow window ||
            window.DocumentUri == null)
        {
            return;
        }
        if (!AnalysisSaveRefreshPolicy.ShouldRefresh(
                AnalysisViewKind.ResourceBindings,
                window.DocumentUri,
                savedFilePath,
                ParseExtensions(GetOptions().FileExtensions)))
        {
            return;
        }
        if (IsAnalysisTargetClosed(window.DocumentUri))
        {
            return;
        }
        if (!resourceBindingsRefreshGate.TryBeginBackgroundRefresh(cause))
        {
            Interlocked.Increment(ref resourceBindingsRequestGeneration);
            window.MarkStale(cause, true);
            return;
        }
        window.BeginRefresh(cause);
        var requestCancellation =
            resourceBindingsRefreshCancellation.BeginNext(cancellationToken);
        await ShowResourceBindingsAsync(
            window.DocumentUri,
            requestCancellation.Token,
            window,
            cancellationToken);
    }

    private async Task ShowPreprocessorExplorerAsync(CancellationToken cancellationToken)
    {
        var uri = await GetActiveDocumentUriAsync(cancellationToken);
        if (uri == null)
        {
            await ShowInformationAsync(
                "Open an HLSL document, then choose HLSL > Preprocessor Explorer.",
                cancellationToken);
            return;
        }
        await BeginAnalysisRefreshIfOpenAsync(
            typeof(PreprocessorExplorerToolWindow),
            AnalysisFreshnessCause.ManualRefresh,
            cancellationToken);
        preprocessorExplorerRefreshGate.EnterExplicitRequest();
        try
        {
            var requestCancellation =
                preprocessorExplorerRefreshCancellation.BeginNext(cancellationToken);
            await ShowPreprocessorExplorerAsync(
                uri,
                requestCancellation.Token,
                null,
                cancellationToken);
        }
        finally
        {
            if (preprocessorExplorerRefreshGate.ExitExplicitRequest(
                    out var pendingCause))
            {
                await RefreshPreprocessorExplorerIfOpenAsync(
                    null,
                    cancellationToken,
                    pendingCause);
            }
        }
    }

    // Mirrors ShowCompilationInfoAsync/ShowResourceBindingsAsync: the
    // generation guard ensures a stale response (e.g. from a superseded
    // variant change or an earlier command invocation) can never overwrite
    // a newer one. A failed or cancelled request never regresses the window
    // to the "open a document" placeholder or leaves it stuck: it keeps the
    // last successful content when one exists, and otherwise shows an
    // explicit error. Issues its own hlsl/preprocessorExplorer request
    // through PreprocessorExplorerBridge -- a distinct protocol request from
    // CompilationInfoBridge, unlike Resource Bindings which reuses it.
    private async Task ShowPreprocessorExplorerAsync(
        Uri uri,
        CancellationToken cancellationToken,
        PreprocessorExplorerToolWindow existingWindow = null,
        CancellationToken ambientCancellationToken = default)
    {
        var generation = Interlocked.Increment(ref preprocessorExplorerRequestGeneration);
        PreprocessorExplorerModel report = null;
        string failureMessage = null;
        try
        {
            report = await PreprocessorExplorerBridge.RequestAsync(uri, cancellationToken);
        }
        catch (OperationCanceledException) when (ambientCancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException)
        {
            failureMessage = "The preprocessor explorer request was cancelled.";
        }
        catch (Exception error)
        {
            failureMessage =
                "Could not retrieve preprocessor explorer information: " + error.Message;
        }
        if (generation != Interlocked.Read(ref preprocessorExplorerRequestGeneration))
        {
            return;
        }
        // The request token may represent the bounded RPC timeout. Once a
        // result or failure message is ready, use only the ambient package
        // token for presentation so a timeout can still be shown to the user.
        await JoinableTaskFactory.SwitchToMainThreadAsync(ambientCancellationToken);
        var window = existingWindow;
        if (window == null)
        {
            window = await ShowToolWindowAsync(
                typeof(PreprocessorExplorerToolWindow),
                0,
                true,
                ambientCancellationToken) as PreprocessorExplorerToolWindow;
        }
        if (generation != Interlocked.Read(ref preprocessorExplorerRequestGeneration))
        {
            return;
        }
        if (failureMessage != null)
        {
            window?.SetError(uri, failureMessage, existingWindow != null);
            return;
        }
        if (report == null)
        {
            window?.SetError(
                uri,
                "The HLSL language server is not ready to provide preprocessor explorer information.",
                existingWindow != null);
            return;
        }
        window?.SetReport(uri, report);
    }

    // Invoked after an active-variant selection or a document save. Only
    // refreshes an already-open window, and only for a save whose saved file
    // matches the window's tracked document, so this cannot start a request
    // storm from unrelated documents or from opening the window for the
    // first time.
    public async Task RefreshPreprocessorExplorerIfOpenAsync(
        string savedFilePath,
        CancellationToken cancellationToken,
        AnalysisFreshnessCause cause = AnalysisFreshnessCause.Unknown)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (await FindToolWindowAsync(
                    typeof(PreprocessorExplorerToolWindow),
                    0,
                    false,
                    cancellationToken)
                is not PreprocessorExplorerToolWindow window ||
            window.DocumentUri == null)
        {
            return;
        }
        if (!AnalysisSaveRefreshPolicy.ShouldRefresh(
                AnalysisViewKind.PreprocessorExplorer,
                window.DocumentUri,
                savedFilePath,
                ParseExtensions(GetOptions().FileExtensions)))
        {
            return;
        }
        if (IsAnalysisTargetClosed(window.DocumentUri))
        {
            return;
        }
        if (!preprocessorExplorerRefreshGate.TryBeginBackgroundRefresh(cause))
        {
            Interlocked.Increment(ref preprocessorExplorerRequestGeneration);
            window.MarkStale(cause, true);
            return;
        }
        window.BeginRefresh(cause);
        var requestCancellation =
            preprocessorExplorerRefreshCancellation.BeginNext(cancellationToken);
        await ShowPreprocessorExplorerAsync(
            window.DocumentUri,
            requestCancellation.Token,
            window,
            cancellationToken);
    }

    private async Task ShowComputeVisualizationAsync(CancellationToken cancellationToken)
    {
        var uri = await GetActiveDocumentUriAsync(cancellationToken);
        if (uri == null)
        {
            await ShowInformationAsync(
                "Open an HLSL document, then choose HLSL > Compute Visualization.",
                cancellationToken);
            return;
        }
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        var existingWindow = await FindToolWindowAsync(
            typeof(ComputeVisualizationToolWindow),
            0,
            false,
            cancellationToken) as ComputeVisualizationToolWindow;
        var options = existingWindow?.SubmittedOptions ?? new ComputeVisualizationOptions();
        await ShowComputeVisualizationExplicitAsync(uri, options, cancellationToken);
    }

    private async Task ShowComputeVisualizationExplicitAsync(
        Uri uri,
        ComputeVisualizationOptions options,
        CancellationToken cancellationToken,
        AnalysisFreshnessCause cause = AnalysisFreshnessCause.ManualRefresh,
        bool pinTracking = true)
    {
        await BeginAnalysisRefreshIfOpenAsync(
            typeof(ComputeVisualizationToolWindow),
            cause,
            cancellationToken,
            pinTracking);
        computeVisualizationRefreshGate.EnterExplicitRequest();
        try
        {
            var requestCancellation =
                computeVisualizationBackgroundRefreshCancellation.BeginNext(cancellationToken);
            await ShowComputeVisualizationAsync(
                uri,
                options,
                requestCancellation.Token,
                null,
                cancellationToken);
        }
        finally
        {
            if (computeVisualizationRefreshGate.ExitExplicitRequest(
                    out var pendingCause))
            {
                await RefreshComputeVisualizationIfOpenAsync(
                    null,
                    cancellationToken,
                    pendingCause);
            }
        }
    }

    private async Task ShowComputeVisualizationAsync(
        Uri uri,
        ComputeVisualizationOptions options,
        CancellationToken cancellationToken,
        ComputeVisualizationToolWindow existingWindow,
        CancellationToken ambientCancellationToken)
    {
        var generation = Interlocked.Increment(ref computeVisualizationRequestGeneration);
        var priorWindow = existingWindow;
        if (priorWindow == null)
        {
            await JoinableTaskFactory.SwitchToMainThreadAsync(ambientCancellationToken);
            priorWindow = await FindToolWindowAsync(
                typeof(ComputeVisualizationToolWindow),
                0,
                false,
                ambientCancellationToken) as ComputeVisualizationToolWindow;
        }
        var preserveContent =
            ComputeVisualizationRefreshLogic.ShouldPreserveContentOnFailure(
                priorWindow?.DisplayedDocumentUri,
                uri);
        priorWindow?.TrackRequest(uri);
        ComputeVisualizationModel report = null;
        string failureMessage = null;
        try
        {
            report = await ComputeVisualizationBridge.RequestAsync(
                uri,
                options,
                cancellationToken);
        }
        catch (OperationCanceledException) when (ambientCancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException)
        {
            failureMessage = "The compute visualization request was cancelled.";
        }
        catch (Exception error)
        {
            failureMessage = "Could not retrieve compute visualization: " + error.Message;
        }
        if (generation != Interlocked.Read(ref computeVisualizationRequestGeneration))
        {
            return;
        }
        await JoinableTaskFactory.SwitchToMainThreadAsync(ambientCancellationToken);
        var window = existingWindow;
        if (ComputeVisualizationRefreshLogic.ShouldRevealToolWindow(existingWindow != null))
        {
            window = await ShowToolWindowAsync(
                typeof(ComputeVisualizationToolWindow),
                0,
                true,
                ambientCancellationToken) as ComputeVisualizationToolWindow;
        }
        if (generation != Interlocked.Read(ref computeVisualizationRequestGeneration))
        {
            return;
        }
        if (failureMessage != null)
        {
            window?.SetError(uri, options, failureMessage, preserveContent);
            if (existingWindow == null)
            {
                window?.SetRequestError(failureMessage);
            }
        }
        else if (report == null)
        {
            const string message =
                "The HLSL language server is not ready to provide compute visualization.";
            window?.SetError(uri, options, message, preserveContent);
            if (existingWindow == null)
            {
                window?.SetRequestError(message);
            }
        }
        else
        {
            window?.SetReport(uri, options, report);
        }
    }

    public async Task RefreshComputeVisualizationIfOpenAsync(
        string savedFilePath,
        CancellationToken cancellationToken,
        AnalysisFreshnessCause cause = AnalysisFreshnessCause.Unknown)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (await FindToolWindowAsync(
                    typeof(ComputeVisualizationToolWindow),
                    0,
                    false,
                    cancellationToken)
                is not ComputeVisualizationToolWindow window ||
            window.DocumentUri == null)
        {
            return;
        }
        if (!AnalysisSaveRefreshPolicy.ShouldRefresh(
                AnalysisViewKind.ComputeVisualization,
                window.DocumentUri,
                savedFilePath,
                ParseExtensions(GetOptions().FileExtensions)))
        {
            return;
        }
        if (IsAnalysisTargetClosed(window.DocumentUri))
        {
            return;
        }
        if (!computeVisualizationRefreshGate.TryBeginBackgroundRefresh(cause))
        {
            Interlocked.Increment(ref computeVisualizationRequestGeneration);
            window.MarkStale(cause, true);
            return;
        }
        var options = ComputeVisualizationRefreshLogic.OptionsForBackgroundRefresh(
            window.SubmittedOptions);
        window.BeginRefresh(cause);
        var refreshCancellation =
            computeVisualizationBackgroundRefreshCancellation.BeginNext(cancellationToken);
        await ShowComputeVisualizationAsync(
            window.DocumentUri,
            options,
            refreshCancellation.Token,
            window,
            cancellationToken);
    }

    private async Task ShowEntryPointDataFlowAsync(CancellationToken cancellationToken)
    {
        var uri = await GetActiveDocumentUriAsync(cancellationToken);
        if (uri == null)
        {
            await ShowInformationAsync(
                "Open an HLSL document, then choose HLSL > Entry-Point Data Flow.",
                cancellationToken);
            return;
        }
        await BeginAnalysisRefreshIfOpenAsync(
            typeof(EntryPointDataFlowToolWindow),
            AnalysisFreshnessCause.ManualRefresh,
            cancellationToken);
        entryPointDataFlowRefreshGate.EnterExplicitRequest();
        try
        {
            var requestCancellation =
                entryPointDataFlowBackgroundRefreshCancellation.BeginNext(cancellationToken);
            await ShowEntryPointDataFlowAsync(
                uri,
                requestCancellation.Token,
                null,
                cancellationToken);
        }
        finally
        {
            if (entryPointDataFlowRefreshGate.ExitExplicitRequest(
                    out var pendingCause))
            {
                // A save/variant/edit refresh arrived while the explicit
                // request above was in flight and was deferred rather than
                // silently dropped (see RefreshEntryPointDataFlowIfOpenAsync
                // below). Replay it once, now that the explicit request this
                // deferral protected has finished, so the window can never
                // be left stale just because a refresh trigger happened to
                // overlap with an explicit command.
                await RefreshEntryPointDataFlowIfOpenAsync(
                    null,
                    cancellationToken,
                    pendingCause);
            }
        }
    }

    // Mirrors ShowCompilationInfoAsync/ShowPreprocessorExplorerAsync: the
    // generation guard ensures a stale response (e.g. from a superseded
    // variant change or an earlier command invocation) can never overwrite
    // a newer one. A failed or cancelled request never regresses the window
    // to the "open a document" placeholder or leaves it stuck: it keeps the
    // last successful content when one exists, and otherwise shows an
    // explicit error. Issues its own hlsl/entryPointDataFlow request through
    // EntryPointDataFlowBridge -- a distinct protocol request, not a
    // different presentation of an existing one.
    private async Task ShowEntryPointDataFlowAsync(
        Uri uri,
        CancellationToken cancellationToken,
        EntryPointDataFlowToolWindow existingWindow = null,
        CancellationToken ambientCancellationToken = default)
    {
        var generation = Interlocked.Increment(ref entryPointDataFlowRequestGeneration);
        // Captured before the request starts, independent of whether the
        // caller already held a window reference: the explicit command
        // below always passes existingWindow: null, even when the window is
        // already open and already showing good content for this exact
        // document, so using existingWindow's null-ness alone to decide
        // whether to preserve that content on failure (as a prior version of
        // this method did) incorrectly erased it on every failed manual
        // retry. This performs a non-creating lookup only -- it must never
        // itself create or show the window, which stays governed solely by
        // ShowToolWindowAsync below, exactly as before.
        var priorWindow = existingWindow;
        if (priorWindow == null)
        {
            await JoinableTaskFactory.SwitchToMainThreadAsync(ambientCancellationToken);
            priorWindow = await FindToolWindowAsync(
                typeof(EntryPointDataFlowToolWindow),
                0,
                false,
                ambientCancellationToken) as EntryPointDataFlowToolWindow;
        }
        var hadMatchingDocument = EntryPointDataFlowRefreshLogic.ShouldPreserveContentOnFailure(
            priorWindow?.DocumentUri,
            uri);
        EntryPointDataFlowModel report = null;
        string failureMessage = null;
        try
        {
            report = await EntryPointDataFlowBridge.RequestAsync(uri, cancellationToken);
        }
        catch (OperationCanceledException) when (ambientCancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException)
        {
            failureMessage = "The entry-point data flow request was cancelled.";
        }
        catch (Exception error)
        {
            failureMessage =
                "Could not retrieve entry-point data flow information: " + error.Message;
        }
        if (generation != Interlocked.Read(ref entryPointDataFlowRequestGeneration))
        {
            return;
        }
        // The request token may represent the bounded RPC timeout. Once a
        // result or failure message is ready, use only the ambient package
        // token for presentation so a timeout can still be shown to the user.
        await JoinableTaskFactory.SwitchToMainThreadAsync(ambientCancellationToken);
        // priorWindow above is used only for the preserve-on-failure
        // decision, never substituted in here: when existingWindow is null
        // (the explicit context-command path), ShowToolWindowAsync must
        // always run so an existing-but-hidden pane is revealed. Using
        // priorWindow as a stand-in for window would let ShowToolWindowAsync
        // be skipped whenever FindToolWindowAsync above happened to find an
        // already-constructed (but not necessarily visible/foregrounded)
        // pane instance, silently leaving a hidden window hidden. A
        // background refresh (which always passes its own existingWindow)
        // is unaffected and still never shows/forces focus.
        var window = existingWindow;
        if (EntryPointDataFlowRefreshLogic.ShouldRevealToolWindow(existingWindow != null))
        {
            window = await ShowToolWindowAsync(
                typeof(EntryPointDataFlowToolWindow),
                0,
                true,
                ambientCancellationToken) as EntryPointDataFlowToolWindow;
        }
        if (generation != Interlocked.Read(ref entryPointDataFlowRequestGeneration))
        {
            return;
        }
        if (failureMessage != null)
        {
            window?.SetError(uri, failureMessage, hadMatchingDocument);
            return;
        }
        if (report == null)
        {
            window?.SetError(
                uri,
                "The HLSL language server is not ready to provide entry-point data flow information.",
                hadMatchingDocument);
            return;
        }
        window?.SetReport(uri, report);
    }

    // Invoked after an active-variant selection, a debounced unsaved edit to
    // any open HLSL/header buffer or shadertoolsconfig.json, or a document
    // save. Only refreshes an already-open window. A save is treated as
    // relevant conservatively, by file type (a configured HLSL/header
    // extension, or shadertoolsconfig.json by name) rather than requiring an
    // exact match against the window's own root document: an #include'd
    // file's declarations/global accesses are reported against their own
    // uri, and a shadertoolsconfig.json change can change the active
    // variant's entry point/defines/include paths entirely, so restricting
    // this to the root document alone would leave the window stale after
    // exactly the changes that matter most. savedFilePath is null for a
    // variant change or a debounced edit refresh, which are always treated
    // as relevant once the window is open.
    public async Task RefreshEntryPointDataFlowIfOpenAsync(
        string savedFilePath,
        CancellationToken cancellationToken,
        AnalysisFreshnessCause cause = AnalysisFreshnessCause.Unknown)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (await FindToolWindowAsync(
                    typeof(EntryPointDataFlowToolWindow),
                    0,
                    false,
                    cancellationToken)
                is not EntryPointDataFlowToolWindow window ||
            window.DocumentUri == null)
        {
            return;
        }
        if (!AnalysisSaveRefreshPolicy.ShouldRefresh(
                AnalysisViewKind.EntryPointDataFlow,
                window.DocumentUri,
                savedFilePath,
                ParseExtensions(GetOptions().FileExtensions)))
        {
            return;
        }
        if (IsAnalysisTargetClosed(window.DocumentUri))
        {
            return;
        }
        if (!entryPointDataFlowRefreshGate.TryBeginBackgroundRefresh(cause))
        {
            Interlocked.Increment(ref entryPointDataFlowRequestGeneration);
            window.MarkStale(cause, true);
            return;
        }
        // Bounds this refresh to a fixed timeout and coalesces it with any
        // earlier still-in-flight background refresh for this same window
        // (a burst of triggers -- e.g. a save arriving while a debounced
        // unsaved-edit refresh is still outstanding -- must not let
        // superseded requests accumulate; see CoalescingBackgroundRefreshCancellation).
        // The ambient cancellationToken is preserved separately below so a
        // timeout/coalescing cancellation still allows presentation logic
        // to run against the real package-lifetime token.
        var refreshCancellation =
            entryPointDataFlowBackgroundRefreshCancellation.BeginNext(cancellationToken);
        window.BeginRefresh(cause);
        await ShowEntryPointDataFlowAsync(
            window.DocumentUri,
            refreshCancellation.Token,
            window,
            cancellationToken);
    }

    // Lazily resolves the plain VS editor services needed to anchor/re-
    // resolve call-hierarchy and memory-layout positions against live buffers
    // (see TryGetOpenCallHierarchyBuffer/CreateRootTrackingPoint below).
    // Resolved at most once per package instance; a failure to resolve any
    // of them is never fatal -- callers simply fall back to a less precise
    // root position source (see CallHierarchyRootPositionResolver). These
    // are plain VS editor services already bundled by Microsoft.VisualStudio.SDK
    // (no new package reference), kept entirely isolated from
    // HlslLanguageClient/StreamJsonRpc, exactly like the rest of this
    // bootstrap package.
    private async Task EnsureCallHierarchyEditorServicesAsync(CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        callHierarchyComponentModel ??=
            await GetServiceAsync(typeof(SComponentModel)) as IComponentModel;
        callHierarchyEditorAdapters ??=
            callHierarchyComponentModel?.GetService<IVsEditorAdaptersFactoryService>();
        callHierarchyRunningDocuments ??=
            await GetServiceAsync(typeof(SVsRunningDocumentTable)) as IVsRunningDocumentTable;
    }

    // Looks up the ITextBuffer currently backing an open document by its
    // file path, independent of which view (if any) is active -- the tracked
    // document need not still be the focused editor when a background
    // refresh runs. Returns null (never throws) when the
    // document is not open, or the editor services above could not be
    // resolved; both are treated identically to "no live buffer to anchor
    // against" by callers.
    private ITextBuffer TryGetOpenCallHierarchyBuffer(Uri documentUri)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (callHierarchyRunningDocuments == null || callHierarchyEditorAdapters == null)
        {
            return null;
        }
        string filePath;
        try
        {
            filePath = documentUri.LocalPath;
        }
        catch (InvalidOperationException)
        {
            return null;
        }
        if (ErrorHandler.Failed(
                callHierarchyRunningDocuments.FindAndLockDocument(
                    (uint)_VSRDTFLAGS.RDT_NoLock,
                    filePath,
                    out _,
                    out _,
                    out var docData,
                    out _)) ||
            docData == IntPtr.Zero)
        {
            return null;
        }
        try
        {
            return Marshal.GetObjectForIUnknown(docData) is IVsTextBuffer adapter
                ? callHierarchyEditorAdapters.GetDocumentBuffer(adapter)
                : null;
        }
        finally
        {
            Marshal.Release(docData);
        }
    }

    // Creates a live anchor for (line, character) in buffer, tracking
    // edits made anywhere before it so a later background refresh can
    // re-resolve the *same* logical position rather than a stale raw
    // line/character (see CallHierarchyRootPositionResolver/
    // CallHierarchyExplorerToolWindow.RootTrackingPoint).
    // PointTrackingMode.Positive mirrors how the editor's own caret tracks
    // a position of interest: text inserted exactly at the anchored offset
    // shifts the anchor forward with it, rather than leaving it stranded
    // before newly typed content. Returns null (never throws) when
    // line/character do not correspond to a valid position in buffer's
    // current snapshot.
    private static ITrackingPoint CreateRootTrackingPoint(ITextBuffer buffer, int line, int character)
    {
        var snapshot = buffer.CurrentSnapshot;
        if (line < 0 || line >= snapshot.LineCount)
        {
            return null;
        }
        var snapshotLine = snapshot.GetLineFromLineNumber(line);
        if (character < 0 || character > snapshotLine.LengthIncludingLineBreak)
        {
            return null;
        }
        var position = snapshotLine.Start.Position + character;
        if (position > snapshot.Length)
        {
            return null;
        }
        return snapshot.CreateTrackingPoint(position, PointTrackingMode.Positive);
    }

    // Translates trackingPoint to a (line, character) tuple against its
    // buffer's CURRENT snapshot -- the whole point of anchoring with an
    // ITrackingPoint is that this reflects any inserts/deletes made
    // anywhere before it since it was created. Returns null (never throws)
    // when trackingPoint is null.
    private static (int Line, int Character)? ResolveTrackedPosition(ITrackingPoint trackingPoint)
    {
        if (trackingPoint == null)
        {
            return null;
        }
        var snapshot = trackingPoint.TextBuffer.CurrentSnapshot;
        var point = trackingPoint.GetPoint(snapshot);
        var containingLine = point.GetContainingLine();
        return (containingLine.LineNumber, point.Position - containingLine.Start.Position);
    }

    // The custom Call Hierarchy surface (HLSL > Call Hierarchy):
    // Visual Studio 17.14's generic ILanguageClient infrastructure does not
    // route the editor's built-in View Call Hierarchy command to any
    // language client, regardless of the callHierarchyProvider capability
    // it advertises -- there is no bespoke call-hierarchy hookup in that
    // SDK the way there is for hover/signature-help/go-to-definition. This
    // command, its tool window, and the three requests below
    // (textDocument/prepareCallHierarchy, then callHierarchy/incomingCalls
    // and callHierarchy/outgoingCalls -- see docs/call-hierarchy.md) are
    // the custom replacement.
    private async Task ShowCallHierarchyAsync(CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (!TryGetActiveHlslEditorContext(
                out var uri,
                out var line,
                out var character,
                out var lines))
        {
            await ShowInformationAsync(
                "Open an HLSL document and place the caret on a function.",
                cancellationToken);
            return;
        }

        // Anchors the explicit request's caret position in the live
        // buffer so later background refreshes can re-resolve the same
        // logical position even after unsaved edits made anywhere before
        // it (see CreateRootTrackingPoint/CallHierarchyRootPositionResolver).
        // A null tracking point (editor services unavailable, or the
        // position somehow out of range) is always safe: refreshes simply
        // fall back to the root item's own SelectionRange, or the raw
        // caret position captured here.
        await EnsureCallHierarchyEditorServicesAsync(cancellationToken);
        var buffer = lines is IVsTextBuffer bufferAdapter
            ? callHierarchyEditorAdapters?.GetDocumentBuffer(bufferAdapter)
            : null;
        var rootTrackingPoint = buffer != null
            ? CreateRootTrackingPoint(buffer, line, character)
            : null;
        await BeginAnalysisRefreshIfOpenAsync(
            typeof(CallHierarchyExplorerToolWindow),
            AnalysisFreshnessCause.ManualRefresh,
            cancellationToken);

        callHierarchyRefreshGate.EnterExplicitRequest();
        try
        {
            var requestCancellation =
                callHierarchyBackgroundRefreshCancellation.BeginNext(cancellationToken);
            await EstablishCallHierarchyRootAsync(
                uri,
                line,
                character,
                rootTrackingPoint,
                requestCancellation.Token,
                cancellationToken);
        }
        finally
        {
            if (callHierarchyRefreshGate.ExitExplicitRequest(
                    out var pendingCause))
            {
                // Mirrors ShowEntryPointDataFlowAsync: a refresh trigger that
                // arrived while this explicit request was in flight was
                // deferred, not dropped -- replay it once now.
                await RefreshCallHierarchyIfOpenAsync(
                    null,
                    cancellationToken,
                    pendingCause);
            }
        }
    }

    // Resolves the callable at (uri, line, character) via
    // textDocument/prepareCallHierarchy, then fetches its incoming and
    // outgoing calls, and always shows/reveals the tool window (an explicit
    // context-command invocation must never leave an existing-but-hidden pane
    // hidden -- there is no "existing window" preservation concern here the
    // way ShowEntryPointDataFlowAsync has, since establishing a *new* root
    // always intentionally replaces whatever was shown before).
    private async Task EstablishCallHierarchyRootAsync(
        Uri uri,
        int line,
        int character,
        ITrackingPoint rootTrackingPoint,
        CancellationToken cancellationToken,
        CancellationToken ambientCancellationToken,
        CallHierarchyExplorerToolWindow existingWindow = null)
    {
        var generation = Interlocked.Increment(ref callHierarchyRequestGeneration);
        IReadOnlyList<CallHierarchyItemModel> prepared = null;
        string failureMessage = null;
        try
        {
            prepared = await CallHierarchyBridge.PrepareAsync(uri, line, character, cancellationToken);
        }
        catch (OperationCanceledException) when (ambientCancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException)
        {
            failureMessage = "The call hierarchy request was cancelled.";
        }
        catch (CallHierarchyContentModifiedException)
        {
            failureMessage = CallHierarchyExplorerDisplay.StaleItemMessage();
        }
        catch (Exception error)
        {
            failureMessage = CallHierarchyExplorerDisplay.RequestFailedMessage(error.Message);
        }
        if (generation != Interlocked.Read(ref callHierarchyRequestGeneration))
        {
            return;
        }

        // The server guarantees at most one element (see
        // docs/call-hierarchy.md), so "the first item, or not-callable" is
        // never actually ambiguous.
        var item = failureMessage == null && prepared != null && prepared.Count > 0
            ? prepared[0]
            : null;
        CallHierarchyFrame frame = null;
        if (failureMessage == null && item != null)
        {
            (frame, failureMessage) = await FetchCallHierarchyFrameAsync(
                item,
                cancellationToken,
                ambientCancellationToken);
        }
        if (generation != Interlocked.Read(ref callHierarchyRequestGeneration))
        {
            return;
        }
        if (failureMessage == null && item == null)
        {
            await JoinableTaskFactory.SwitchToMainThreadAsync(ambientCancellationToken);
            var notCallableWindow = existingWindow ??
                await FindToolWindowAsync(
                    typeof(CallHierarchyExplorerToolWindow),
                    0,
                    false,
                    ambientCancellationToken)
                as CallHierarchyExplorerToolWindow;
            if (notCallableWindow != null)
            {
                notCallableWindow.SetNotCallable(
                    uri,
                    line,
                    character,
                    rootTrackingPoint);
            }
            if (existingWindow == null)
            {
                await ShowInformationAsync(
                    "No callable symbol was found at the selected position.",
                    ambientCancellationToken);
            }
            return;
        }

        await JoinableTaskFactory.SwitchToMainThreadAsync(ambientCancellationToken);
        var window = existingWindow;
        if (window == null)
        {
            window = await ShowToolWindowAsync(
                typeof(CallHierarchyExplorerToolWindow),
                0,
                true,
                ambientCancellationToken) as CallHierarchyExplorerToolWindow;
        }
        WireCallHierarchyWindow(window, ambientCancellationToken);
        if (generation != Interlocked.Read(ref callHierarchyRequestGeneration))
        {
            return;
        }
        if (failureMessage != null)
        {
            if (window?.CurrentItem != null)
            {
                if (existingWindow != null)
                {
                    window.SetFailedRetarget(
                        uri,
                        line,
                        character,
                        rootTrackingPoint,
                        failureMessage);
                }
                else
                {
                    window.PreserveTrackedRootAfterFailedReplacement(
                        uri,
                        line,
                        character);
                    window.SetBannerOnCurrent(failureMessage);
                }
            }
            else
            {
                window?.SetGlobalError(
                    failureMessage,
                    uri,
                    line,
                    character,
                    rootTrackingPoint);
            }
            return;
        }
        window?.SetRoot(uri, line, character, frame, rootTrackingPoint);
    }

    // Invoked when the user clicks "Explore calls" on a caller/callee row:
    // re-centers the tool window on that item by fetching its own
    // incoming/outgoing calls. The item's opaque `data` (identity envelope)
    // is already known from the previous response, so no new
    // prepareCallHierarchy call is needed -- see docs/call-hierarchy.md.
    // Treated as an explicit, gated, timeout-bounded operation exactly like
    // the context command itself, since a concurrent background refresh must
    // not silently drop or be dropped by it. A failed drill-in never
    // mutates the tool window's persisted state (unlike a failed
    // root/refresh): the currently displayed frame is left exactly as-is
    // and the failure is surfaced through a one-off message box, so a bad
    // drill-in attempt can never corrupt or blank out an otherwise good
    // view.
    internal async Task PerformCallHierarchyDrillInAsync(
        CallHierarchyItemModel item,
        CallHierarchySection section,
        CancellationToken cancellationToken)
    {
        if (item == null)
        {
            return;
        }
        // Captured before the network round-trip below so a concurrent
        // Back click (or another drill-in/refresh) that changes the stack
        // while this is in flight can be detected and rejected once the
        // frame is ready to push (see the NavigationRevision check before
        // PushFrame). Uses a non-creating lookup, mirroring
        // RefreshCallHierarchyIfOpenAsync -- a drill-in only ever
        // originates from a row click within an already-open window, so
        // windowBeforeFetch is null only in the defensive/unreachable case
        // of the window having been closed between the click and here; ??
        // 0 matches a freshly (re-)created window's own starting
        // revision, so that edge case still pushes correctly rather than
        // spuriously rejecting the very first drill-in.
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        var windowBeforeFetch = await FindToolWindowAsync(
                typeof(CallHierarchyExplorerToolWindow),
                0,
                false,
                cancellationToken)
            as CallHierarchyExplorerToolWindow;
        var revisionAtStart = windowBeforeFetch?.NavigationRevision ?? 0;
        callHierarchyRefreshGate.EnterExplicitRequest();
        try
        {
            var requestCancellation =
                callHierarchyBackgroundRefreshCancellation.BeginNext(cancellationToken);
            var generation = Interlocked.Increment(ref callHierarchyRequestGeneration);
            var (frame, failureMessage) = await FetchCallHierarchyFrameAsync(
                item,
                requestCancellation.Token,
                cancellationToken);
            if (generation != Interlocked.Read(ref callHierarchyRequestGeneration))
            {
                return;
            }
            await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
            if (generation != Interlocked.Read(ref callHierarchyRequestGeneration))
            {
                return;
            }
            if (failureMessage != null)
            {
                VsShellUtilities.ShowMessageBox(
                    this,
                    failureMessage,
                    "HLSL Call Hierarchy",
                    OLEMSGICON.OLEMSGICON_WARNING,
                    OLEMSGBUTTON.OLEMSGBUTTON_OK,
                    OLEMSGDEFBUTTON.OLEMSGDEFBUTTON_FIRST);
                return;
            }
            var window = await ShowToolWindowAsync(
                typeof(CallHierarchyExplorerToolWindow),
                0,
                true,
                cancellationToken) as CallHierarchyExplorerToolWindow;
            WireCallHierarchyWindow(window, cancellationToken);
            if (generation != Interlocked.Read(ref callHierarchyRequestGeneration))
            {
                return;
            }
            if (window != null && window.NavigationRevision != revisionAtStart)
            {
                // The user navigated (most importantly, pressed Back)
                // while this drill-in's network round-trip was in
                // flight: pushing this frame now would land on top of
                // a stack the user has already moved away from,
                // silently overriding their navigation. Discard it
                // rather than applying a decision made against an
                // outdated stack.
                return;
            }
            window?.PushFrame(item, section, frame);
        }
        finally
        {
            if (callHierarchyRefreshGate.ExitExplicitRequest(
                    out var pendingCause))
            {
                await RefreshCallHierarchyIfOpenAsync(
                    null,
                    cancellationToken,
                    pendingCause);
            }
        }
    }

    // Invoked after an active-variant selection, a document save, or a
    // debounced unsaved edit to any open HLSL/header buffer or
    // shadertoolsconfig.json (see RefreshVariantDependentWindows/
    // OnAfterSave/DebounceUnsavedHlslBufferRefreshAsync in
    // HlslLspActivator). Every one of those triggers is exactly the kind of
    // change that bumps the root item's own generation (see
    // docs/call-hierarchy.md), so simply round-tripping the previously
    // resolved CallHierarchyItem's opaque `data` back into incomingCalls/
    // outgoingCalls (as an earlier version of this method did) would be
    // guaranteed to fail with ContentModified every single time this
    // refresh actually runs -- it could never succeed once triggered. This
    // re-runs textDocument/prepareCallHierarchy at the originally captured
    // root position to obtain a genuinely fresh item, confirms it is still
    // the *same* declaration as the currently displayed root (see
    // CallHierarchyItemIdentity.IsSameCallable -- a tracking point can land
    // on a different callable after the original was deleted/replaced, or
    // after the document was closed and reopened with different content),
    // then, if the user had drilled deeper than the root, attempts to
    // relocate each drilled item within its parent's freshly fetched list
    // by stable cross-generation identity (see CallHierarchyItemIdentity).
    // The rebuild is all-or-nothing: on full success the entire stack is
    // replaced in place (preserving depth); if the root itself is no
    // longer callable, an authoritative not-callable result is shown; if
    // the root's identity cannot be confirmed, or on any other failure,
    // the last successful content is preserved with a banner; if only the
    // drilled path can't be relocated, the view resets to the fresh root
    // with an explanatory banner rather than showing a partially-rebuilt or
    // possibly-wrong path. Only refreshes an already-open window with an
    // established root; never creates or shows one. Uses the same broad,
    // conservative relevance test as RefreshEntryPointDataFlowIfOpenAsync (a
    // configured HLSL/header extension, or shadertoolsconfig.json by name)
    // since an #include'd dependency or a variant/config change can change
    // the current item's own callers/callees or generation without the
    // currently displayed item's own document changing.
    public async Task RefreshCallHierarchyIfOpenAsync(
        string savedFilePath,
        CancellationToken cancellationToken,
        AnalysisFreshnessCause cause = AnalysisFreshnessCause.Unknown)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (await FindToolWindowAsync(
                    typeof(CallHierarchyExplorerToolWindow),
                    0,
                    false,
                    cancellationToken)
                is not CallHierarchyExplorerToolWindow window ||
            window.RootDocumentUri == null)
        {
            return;
        }
        if (!AnalysisSaveRefreshPolicy.ShouldRefresh(
                AnalysisViewKind.CallHierarchy,
                window.RootDocumentUri,
                savedFilePath,
                ParseExtensions(GetOptions().FileExtensions)))
        {
            return;
        }
        if (IsAnalysisTargetClosed(window.RootDocumentUri))
        {
            return;
        }
        if (window.RetargetPending)
        {
            if (window.TrackingMode == AnalysisTrackingMode.FollowActiveShader)
            {
                if (!TryGetActiveHlslEditorContext(
                        out var activeUri,
                        out var activeLine,
                        out var activeCharacter,
                        out var activeLines))
                {
                    InvalidateAnalysisTrackingRequest(AnalysisViewKind.CallHierarchy);
                    window.MarkStale(AnalysisFreshnessCause.ActiveShaderUnavailable);
                    return;
                }
                await RetargetCallHierarchyAsync(
                    window,
                    activeUri,
                    activeLine,
                    activeCharacter,
                    activeLines,
                    cancellationToken);
            }
            else
            {
                await RetryPendingCallHierarchyRetargetAsync(
                    window,
                    cancellationToken);
            }
            return;
        }
        if (!callHierarchyRefreshGate.TryBeginBackgroundRefresh(cause))
        {
            Interlocked.Increment(ref callHierarchyRequestGeneration);
            window.MarkStale(cause, true);
            return;
        }

        // Captured before any request is (re-)issued: the raw root
        // position/document/drill-in path last established, and the
        // navigation revision at that exact moment (see
        // CallHierarchyExplorerState.Revision). If the user navigates
        // (most importantly, presses Back) before this refresh finishes,
        // the revision will have moved on by the time this method is ready
        // to apply its result, and that result must then be discarded
        // rather than silently overwriting/undoing the user's navigation
        // (checked just before committing, below).
        var rootUri = window.RootDocumentUri;
        var rootLine = window.RootLine;
        var rootCharacter = window.RootCharacter;
        var path = window.CapturePathSteps();
        var revisionAtStart = window.NavigationRevision;

        // Resolve the position to re-run prepareCallHierarchy at: prefer a
        // live tracking-point translation, which alone correctly follows
        // unsaved inserts/deletes made anywhere before the root since it
        // was established; fall back to the last resolved root item's own
        // compiler-supplied SelectionRange when the tracking point's
        // buffer is no longer the live buffer for this document (closed
        // and reopened, or externally reloaded -- a new buffer identity
        // the old tracking point cannot follow); finally fall back to the
        // raw position captured at root establishment (see
        // CallHierarchyRootPositionResolver).
        await EnsureCallHierarchyEditorServicesAsync(cancellationToken);
        var liveBuffer = TryGetOpenCallHierarchyBuffer(rootUri);
        var trackedPosition =
            liveBuffer != null &&
            window.RootTrackingPoint != null &&
            ReferenceEquals(liveBuffer, window.RootTrackingPoint.TextBuffer)
                ? ResolveTrackedPosition(window.RootTrackingPoint)
                : null;
        (int Line, int Character)? fallbackSelectionStart = null;
        var rootItemSelectionStart = window.RootItem?.SelectionRange?.Start;
        if (rootItemSelectionStart != null)
        {
            fallbackSelectionStart = ((int)rootItemSelectionStart.Line, (int)rootItemSelectionStart.Character);
        }
        var (resolvedLine, resolvedCharacter) = CallHierarchyRootPositionResolver.ResolveRefreshPosition(
            trackedPosition,
            fallbackSelectionStart,
            rootLine,
            rootCharacter);

        // Bounds this refresh to a fixed timeout and coalesces it with any
        // earlier still-in-flight background refresh for this window (see
        // CoalescingBackgroundRefreshCancellation) so a burst of triggers
        // cannot accumulate unboundedly in-flight requests.
        var refreshCancellation =
            callHierarchyBackgroundRefreshCancellation.BeginNext(cancellationToken);
        window.BeginRefresh(cause);
        var token = refreshCancellation.Token;
        var generation = Interlocked.Increment(ref callHierarchyRequestGeneration);

        IReadOnlyList<CallHierarchyItemModel> prepared = null;
        string failureMessage = null;
        try
        {
            prepared = await CallHierarchyBridge.PrepareAsync(rootUri, resolvedLine, resolvedCharacter, token);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException)
        {
            failureMessage = "The call hierarchy request was cancelled.";
        }
        catch (CallHierarchyContentModifiedException)
        {
            failureMessage = CallHierarchyExplorerDisplay.StaleItemMessage();
        }
        catch (Exception error)
        {
            failureMessage = CallHierarchyExplorerDisplay.RequestFailedMessage(error.Message);
        }
        if (generation != Interlocked.Read(ref callHierarchyRequestGeneration))
        {
            // Never treat a guaranteed-stale, superseded response as
            // successful: a newer request (explicit or background) has
            // already taken over, so this one must simply stop without
            // touching the window at all.
            return;
        }

        // The server guarantees at most one element (see
        // docs/call-hierarchy.md).
        var rootItem = failureMessage == null && prepared != null && prepared.Count > 0
            ? prepared[0]
            : null;

        // Before trusting this freshly re-prepared item as a continuation
        // of the currently displayed root, confirm it is actually the
        // *same* declaration (see CallHierarchyItemIdentity.IsSameCallable)
        // -- a tracking point (or its fallbacks) can land on an unrelated
        // callable after the original was deleted, replaced by a
        // differently-named-or-typed declaration, or the document was
        // closed and reopened with different content at that position.
        // Reusing failureMessage here deliberately routes this into the
        // exact same "preserve last successful content, show a banner"
        // handling as any other transient failure below -- this is not a
        // transient error, but the safe behavior (never silently switch
        // roots) is identical.
        if (failureMessage == null &&
            rootItem != null &&
            window.RootItem != null &&
            !CallHierarchyItemIdentity.IsSameCallable(window.RootItem, rootItem))
        {
            failureMessage = CallHierarchyExplorerDisplay.RootIdentityChangedMessage();
        }

        CallHierarchyFrame rootFrame = null;
        if (failureMessage == null && rootItem != null)
        {
            (rootFrame, failureMessage) = await FetchCallHierarchyFrameAsync(rootItem, token, cancellationToken);
        }
        if (generation != Interlocked.Read(ref callHierarchyRequestGeneration))
        {
            return;
        }

        // A fresh anchor for the root actually resolved above, used to
        // update the window's tracked position going forward (a stale
        // caret-derived resolvedLine/resolvedCharacter should not linger
        // as the anchor once a compiler-verified SelectionRange for the
        // same declaration is available). liveBuffer above is still the
        // correct buffer to anchor against: rootUri itself never changes
        // across a refresh, only the position within it might.
        var anchorLine = rootItem?.SelectionRange?.Start != null
            ? (int)rootItem.SelectionRange.Start.Line
            : resolvedLine;
        var anchorCharacter = rootItem?.SelectionRange?.Start != null
            ? (int)rootItem.SelectionRange.Start.Character
            : resolvedCharacter;
        var freshTrackingPoint = liveBuffer != null
            ? CreateRootTrackingPoint(liveBuffer, anchorLine, anchorCharacter)
            : null;

        // Attempt to rebuild the user's drill-in path (if any) underneath
        // the freshly fetched root, one step at a time. All-or-nothing: any
        // step that cannot be relocated (removed/renamed declaration) or
        // whose own frame cannot be fetched abandons the rebuild in favor
        // of resetting to the fresh root with an explanation.
        List<CallHierarchyFrame> rebuiltFrames = null;
        List<CallHierarchyPathStep> rebuiltSteps = null;
        string rebuildFailureMessage = null;
        if (failureMessage == null && rootFrame != null && path.Count > 0)
        {
            rebuiltFrames = new List<CallHierarchyFrame> { rootFrame };
            rebuiltSteps = new List<CallHierarchyPathStep>();
            var parentFrame = rootFrame;
            foreach (var step in path)
            {
                var matched = CallHierarchyItemIdentity.FindMatch(parentFrame, step);
                if (matched == null)
                {
                    rebuiltFrames = null;
                    rebuildFailureMessage = CallHierarchyExplorerDisplay.PathNotRelocatedMessage();
                    break;
                }
                var (stepFrame, stepFailureMessage) =
                    await FetchCallHierarchyFrameAsync(matched, token, cancellationToken);
                if (generation != Interlocked.Read(ref callHierarchyRequestGeneration))
                {
                    return;
                }
                if (stepFrame == null)
                {
                    rebuiltFrames = null;
                    rebuildFailureMessage = stepFailureMessage
                        ?? CallHierarchyExplorerDisplay.PathNotRelocatedMessage();
                    break;
                }
                rebuiltFrames.Add(stepFrame);
                rebuiltSteps.Add(step);
                parentFrame = stepFrame;
            }
        }

        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (generation != Interlocked.Read(ref callHierarchyRequestGeneration))
        {
            return;
        }
        if (window.NavigationRevision != revisionAtStart)
        {
            // The user navigated (most importantly, pressed Back) while
            // this refresh was in flight: applying its result now --
            // whatever it is -- would silently overwrite or undo that
            // navigation. Discard it outright rather than reconciling a
            // now-outdated result with a since-changed stack; the next
            // refresh trigger (or a manual context command) will re-capture
            // a correct revision and path from the user's actual current
            // position.
            window.DiscardRefreshAfterNavigation(cause);
            return;
        }
        if (failureMessage == null && rootItem == null)
        {
            // The previously callable root no longer resolves to anything
            // callable (e.g. the function was deleted or renamed) -- an
            // authoritative result, not a transient failure, exactly like
            // EstablishCallHierarchyRootAsync's own not-callable handling.
            window.SetNotCallable(
                rootUri,
                resolvedLine,
                resolvedCharacter,
                freshTrackingPoint);
            return;
        }
        if (failureMessage != null)
        {
            // Preserve the last successful content on a transient
            // failure/stale response -- only overlay a banner, matching the
            // "preserve last successful content on transient errors"
            // contract established for entry-point data flow.
            window.SetBannerOnCurrent(failureMessage);
            return;
        }
        if (rebuiltFrames != null)
        {
            // The full drilled path was successfully relocated underneath
            // the fresh root: replace the entire stack in one step,
            // preserving the user's drill-in depth and clearing any earlier
            // banner, since this is a fully fresh, successful result. The
            // root anchor is refreshed too, so a *future* refresh tracks
            // forward from here rather than the now-outdated position.
            window.UpdateRootAnchor(rootUri, anchorLine, anchorCharacter, freshTrackingPoint);
            window.ReplaceAllFrames(rebuiltFrames, rebuiltSteps);
            return;
        }
        // Either there was no drill-in path to rebuild (a root-only refresh
        // fully succeeds with the fresh root alone) or the path could not be
        // relocated: reset to the fresh root either way, but only attach an
        // explanatory banner when a path actually failed to relocate.
        window.SetRoot(rootUri, anchorLine, anchorCharacter, rootFrame, freshTrackingPoint);
        if (rebuildFailureMessage != null)
        {
            window.SetBannerOnCurrent(rebuildFailureMessage, false);
        }
    }

    private static async Task<(CallHierarchyFrame Frame, string FailureMessage)> FetchCallHierarchyFrameAsync(
        CallHierarchyItemModel item,
        CancellationToken cancellationToken,
        CancellationToken ambientCancellationToken)
    {
        try
        {
            var incomingCalls =
                await CallHierarchyBridge.RequestIncomingCallsAsync(item, cancellationToken);
            var outgoingCalls =
                await CallHierarchyBridge.RequestOutgoingCallsAsync(item, cancellationToken);
            return (new CallHierarchyFrame(item, incomingCalls, outgoingCalls), null);
        }
        catch (OperationCanceledException) when (ambientCancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException)
        {
            return (null, "The call hierarchy request was cancelled.");
        }
        catch (CallHierarchyContentModifiedException)
        {
            return (null, CallHierarchyExplorerDisplay.StaleItemMessage());
        }
        catch (Exception error)
        {
            return (null, CallHierarchyExplorerDisplay.RequestFailedMessage(error.Message));
        }
    }

    // Wires the tool window's Back/"Explore calls" interactions to this
    // package exactly once per window instance (EnsureWired itself is
    // idempotent, since ShowToolWindowAsync/FindToolWindowAsync return the
    // same singleton pane across every invocation).
    private void WireCallHierarchyWindow(
        CallHierarchyExplorerToolWindow window,
        CancellationToken cancellationToken)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        window?.EnsureWired(
            (item, section) => JoinableTaskFactory.RunAsync(
                    () => PerformCallHierarchyDrillInAsync(item, section, cancellationToken))
                .FileAndForget("HlslLsp/CallHierarchyDrillIn"));
    }

    private async Task SelectVariantAsync(CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        if (!TryGetActiveHlslEditorContext(
                out var uri,
                out var line,
                out var character,
                out _))
        {
            await ShowInformationAsync(
                "Open an HLSL document to select a shader variant.",
                cancellationToken);
            return;
        }
        VariantListModel variants = null;
        try
        {
            variants = await VariantBridge.ListAsync(uri, cancellationToken);
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception error)
        {
            await ShowInformationAsync(
                "Could not load shader variants: " + error.Message,
                cancellationToken);
            return;
        }
        string callableName = null;
        try
        {
            var prepared = await CallHierarchyBridge.PrepareAsync(
                uri,
                line,
                character,
                cancellationToken);
            if (prepared != null && prepared.Count > 0)
            {
                callableName = prepared[0].Name;
            }
        }
        catch (OperationCanceledException)
        {
            throw;
        }
        catch (Exception error)
        {
            ActivityLog.LogWarning(
                nameof(HlslBootstrapPackage),
                "Could not resolve the shader-variant caret context: " + error);
        }
        variants = VariantSelection.ForContext(variants, callableName);
        if ((variants?.Variants == null || variants.Variants.Count == 0) &&
            string.IsNullOrEmpty(variants?.ActiveVariant))
        {
            await ShowInformationAsync(
                VariantBridge.IsAvailable
                    ? "No shader variants apply to this file or selected entry point."
                    : "Open an HLSL document so the language server can load shader variants.",
                cancellationToken);
            return;
        }

        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        var dialog = new VariantSelectionDialog(variants);
        if (dialog.ShowModal() == true)
        {
            await VariantBridge.SetActiveAsync(dialog.SelectedVariant, cancellationToken);
        }
    }

    private async Task<Uri> GetActiveDocumentUriAsync(CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        return TryGetActiveHlslEditorContext(out var uri, out _, out _, out _)
            ? uri
            : null;
    }

    private void OnHlslContextCommandBeforeQueryStatus(object sender, EventArgs eventArgs)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        _ = eventArgs;
        if (sender is OleMenuCommand command)
        {
            var hlslEditor = TryGetActiveHlslEditorContext(
                out var uri,
                out var line,
                out var character,
                out _);
            HlslCommandContextModel context = null;
            var contextKnown = hlslEditor &&
                HlslCommandContextCache.TryGet(
                    uri,
                    line,
                    character,
                    out context);
            var presentation = HlslCommandPresentation.Evaluate(
                CommandKind(command.CommandID.ID),
                hlslEditor,
                contextKnown,
                context);
            command.Visible = presentation.Visible;
            command.Enabled = presentation.Enabled;
            command.Text = presentation.Text;
        }
    }

    private static HlslCommandKind CommandKind(int commandId)
    {
        switch (commandId)
        {
            case 0x0100:
                return HlslCommandKind.MemoryLayout;
            case 0x0101:
                return HlslCommandKind.SelectVariant;
            case 0x0102:
                return HlslCommandKind.Compilation;
            case 0x0103:
                return HlslCommandKind.ResourceBindings;
            case 0x0104:
                return HlslCommandKind.PreprocessorExplorer;
            case 0x0105:
                return HlslCommandKind.EntryPointDataFlow;
            case 0x0106:
                return HlslCommandKind.CallHierarchy;
            case 0x0107:
                return HlslCommandKind.ComputeVisualization;
            default:
                throw new ArgumentOutOfRangeException(
                    nameof(commandId),
                    commandId,
                    "Unknown HLSL command identifier.");
        }
    }

    private bool TryGetActiveHlslEditorContext(
        out Uri uri,
        out int line,
        out int character,
        out IVsTextLines lines)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        uri = null;
        line = 0;
        character = 0;
        lines = null;
        if (commandTextManager == null ||
            ErrorHandler.Failed(commandTextManager.GetActiveView(1, null, out var view)) ||
            view == null ||
            ErrorHandler.Failed(view.GetCaretPos(out line, out character)) ||
            ErrorHandler.Failed(view.GetBuffer(out var bufferLines)) ||
            bufferLines is not IVsUserData userData)
        {
            return false;
        }
        var monikerKey = VSConstants.VsTextBufferUserDataGuid.VsBufferMoniker_guid;
        if (ErrorHandler.Failed(userData.GetData(ref monikerKey, out var value)) ||
            value is not string moniker)
        {
            return false;
        }
        var extension = Path.GetExtension(moniker);
        if (!string.Equals(extension, ".hlsl", StringComparison.OrdinalIgnoreCase) &&
            !string.Equals(extension, ".hlsli", StringComparison.OrdinalIgnoreCase) &&
            !ParseExtensions(GetOptions().FileExtensions).Contains(
                extension,
                StringComparer.OrdinalIgnoreCase))
        {
            return false;
        }
        uri = new Uri(Path.GetFullPath(moniker));
        lines = bufferLines;
        return true;
    }

    private async Task ShowInformationAsync(string message, CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        VsShellUtilities.ShowMessageBox(
            this,
            message,
            "HLSL-LSP",
            OLEMSGICON.OLEMSGICON_INFO,
            OLEMSGBUTTON.OLEMSGBUTTON_OK,
            OLEMSGDEFBUTTON.OLEMSGDEFBUTTON_FIRST);
    }

    private async Task TryActivateLanguageClientAsync(
        CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        var configuredExtensions = new HashSet<string>(
            ParseExtensions(GetOptions().FileExtensions),
            StringComparer.OrdinalIgnoreCase);
        lock (Gate)
        {
            if (activationStarted ||
                !PendingDocuments.Any(
                    path => configuredExtensions.Contains(Path.GetExtension(path))))
            {
                return;
            }
            activationStarted = true;
        }

        try
        {
            await LoadLanguageClientAsync(cancellationToken);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            lock (Gate)
            {
                activationStarted = false;
            }
        }
        catch (Exception error)
        {
            ActivityLog.LogError(nameof(HlslBootstrapPackage), error.ToString());
            lock (Gate)
            {
                activationStarted = false;
            }
        }
    }

    private async Task LoadLanguageClientAsync(
        CancellationToken cancellationToken)
    {
        await JoinableTaskFactory.SwitchToMainThreadAsync(cancellationToken);
        var directory = Path.GetDirectoryName(GetType().Assembly.Location);
        var assembly = Assembly.LoadFrom(
            Path.Combine(directory, "Client", "HlslLsp.VisualStudio.dll"));
        var activator = assembly.GetType(
            "HlslLsp.VisualStudio.HlslLspActivator",
            throwOnError: true);
        var activate = activator.GetMethod(
            "ActivateAsync",
            BindingFlags.Public | BindingFlags.Static);
        if (activate == null)
        {
            throw new InvalidOperationException(
                "The HLSL language client activation entry point is unavailable.");
        }

        var task = activate.Invoke(
            null,
            new object[] { this, cancellationToken }) as Task;
        if (task == null)
        {
            throw new InvalidOperationException(
                "The HLSL language client did not return an activation task.");
        }
        await task;
    }

    public static IEnumerable<string> ParseExtensions(string value)
    {
        return (value ?? string.Empty)
            .Split(new[] { ';', ',', ' ' }, StringSplitOptions.RemoveEmptyEntries)
            .Select(extension => extension.Trim())
            .Where(extension => extension.Length > 0)
            .Select(extension => extension[0] == '.' ? extension : "." + extension)
            .Distinct(StringComparer.OrdinalIgnoreCase);
    }

    internal static void NotifyOptionsChanged()
    {
        HlslCommandContextBridge.Invalidate();
        OptionsChanged?.Invoke();
        HlslBootstrapPackage package;
        lock (Gate)
        {
            package = instance;
        }
        package?.JoinableTaskFactory.RunAsync(
                () => package.TryActivateLanguageClientAsync(package.DisposalToken))
            .FileAndForget("HlslLsp/ApplyBootstrapOptions");
    }
}

public sealed class HlslOptionsSnapshot
{
    public HlslOptionsSnapshot(
        string fileExtensions,
        string languageVersion,
        string dxcRuntimeDirectory,
        InlayHintOptionsSnapshot inlayHints)
    {
        FileExtensions = fileExtensions;
        LanguageVersion = languageVersion;
        DxcRuntimeDirectory = dxcRuntimeDirectory;
        InlayHints = inlayHints;
    }

    public string FileExtensions { get; }

    public string LanguageVersion { get; }

    public string DxcRuntimeDirectory { get; }

    public InlayHintOptionsSnapshot InlayHints { get; }
}

public sealed class InlayHintOptionsSnapshot
{
    public InlayHintOptionsSnapshot(
        bool types,
        bool parameters,
        bool matrixOrientation,
        bool registers,
        bool packedOffsets,
        bool arrayStrides,
        bool activeVariant)
    {
        Types = types;
        Parameters = parameters;
        MatrixOrientation = matrixOrientation;
        Registers = registers;
        PackedOffsets = packedOffsets;
        ArrayStrides = arrayStrides;
        ActiveVariant = activeVariant;
    }

    public bool Types { get; }
    public bool Parameters { get; }
    public bool MatrixOrientation { get; }
    public bool Registers { get; }
    public bool PackedOffsets { get; }
    public bool ArrayStrides { get; }
    public bool ActiveVariant { get; }
}

[TypeDescriptionProvider(typeof(HlslOptionsTypeDescriptionProvider))]
public sealed class HlslOptionsPage : DialogPage
{
    private string fileExtensions = ".hlsl;.hlsli;.usf";
    private string languageVersion = "2021";
    private string dxcRuntimeDirectory = "";
    private bool inlayHintTypes = true;
    private bool inlayHintParameters = true;
    private bool inlayHintMatrixOrientation;
    private bool inlayHintRegisters;
    private bool inlayHintPackedOffsets;
    private bool inlayHintArrayStrides;
    private bool inlayHintActiveVariant = true;

    [Category("Files")]
    [System.ComponentModel.DisplayName("HLSL file extensions")]
    [Description(
        "Semicolon-separated file extensions to treat as HLSL. " +
        "The built-in .hlsl and .hlsli extensions are always supported.")]
    public string FileExtensions
    {
        get => fileExtensions;
        set
        {
            if (string.Equals(fileExtensions, value, StringComparison.Ordinal))
            {
                return;
            }
            fileExtensions = value;
            HlslBootstrapPackage.NotifyOptionsChanged();
        }
    }

    [Category("Language")]
    [System.ComponentModel.DisplayName("Default HLSL language version")]
    [Description(
        "The default DXC -HV value, such as 2016, 2017, 2018, 2021, or 202x. " +
        "A shadertoolsconfig.json languageVersion setting takes precedence.")]
    public string LanguageVersion
    {
        get => languageVersion;
        set
        {
            if (string.Equals(languageVersion, value, StringComparison.Ordinal))
            {
                return;
            }
            languageVersion = value;
            HlslBootstrapPackage.NotifyOptionsChanged();
        }
    }

    [Category("DXC")]
    [System.ComponentModel.DisplayName("DXC runtime directory")]
    [Description(
        "Directory containing a compatible DXC runtime to load instead of the " +
        "bundled one (dxcompiler.dll and dxil.dll). An explicit value overrides " +
        "shadertoolsconfig.json. Leave empty to use the bundled runtime. " +
        "Changing this restarts the language server.")]
    public string DxcRuntimeDirectory
    {
        get => dxcRuntimeDirectory;
        set
        {
            if (string.Equals(dxcRuntimeDirectory, value, StringComparison.Ordinal))
            {
                return;
            }
            dxcRuntimeDirectory = value;
            HlslBootstrapPackage.NotifyOptionsChanged();
        }
    }

    [Category("Inlay hints")]
    [System.ComponentModel.DisplayName("Inferred types")]
    [Description("Show compiler-inferred type hints.")]
    public bool InlayHintTypes
    {
        get => inlayHintTypes;
        set => SetOption(ref inlayHintTypes, value);
    }

    [Category("Inlay hints")]
    [System.ComponentModel.DisplayName("Parameter names")]
    [Description("Show unambiguous overload parameter-name hints at call sites.")]
    public bool InlayHintParameters
    {
        get => inlayHintParameters;
        set => SetOption(ref inlayHintParameters, value);
    }

    [Category("Inlay hints")]
    [System.ComponentModel.DisplayName("Matrix orientation")]
    [Description("Show compiler-derived row-major or column-major matrix orientation hints.")]
    public bool InlayHintMatrixOrientation
    {
        get => inlayHintMatrixOrientation;
        set => SetOption(ref inlayHintMatrixOrientation, value);
    }

    [Category("Inlay hints")]
    [System.ComponentModel.DisplayName("Resource registers")]
    [Description("Show reflected resource register and space hints.")]
    public bool InlayHintRegisters
    {
        get => inlayHintRegisters;
        set => SetOption(ref inlayHintRegisters, value);
    }

    [Category("Inlay hints")]
    [System.ComponentModel.DisplayName("Packed offsets")]
    [Description("Show compiler-derived constant-buffer packed-offset hints.")]
    public bool InlayHintPackedOffsets
    {
        get => inlayHintPackedOffsets;
        set => SetOption(ref inlayHintPackedOffsets, value);
    }

    [Category("Inlay hints")]
    [System.ComponentModel.DisplayName("Array strides")]
    [Description("Show compiler-derived array-stride hints.")]
    public bool InlayHintArrayStrides
    {
        get => inlayHintArrayStrides;
        set => SetOption(ref inlayHintArrayStrides, value);
    }

    [Category("Inlay hints")]
    [System.ComponentModel.DisplayName("Active variant")]
    [Description("Show the selected compilation variant where it affects the current shader.")]
    public bool InlayHintActiveVariant
    {
        get => inlayHintActiveVariant;
        set => SetOption(ref inlayHintActiveVariant, value);
    }

    private static void SetOption(ref bool field, bool value)
    {
        if (field == value)
        {
            return;
        }
        field = value;
        HlslBootstrapPackage.NotifyOptionsChanged();
    }

    internal sealed class HlslOptionsTypeDescriptionProvider : TypeDescriptionProvider
    {
        private const string AttributeTypeName =
            "Microsoft.VisualStudio.Shell.UnifiedSettingsMonikerAttribute, " +
            "Microsoft.VisualStudio.Shell.15.0";

        public HlslOptionsTypeDescriptionProvider()
            : base(TypeDescriptor.GetProvider(typeof(DialogPage)))
        {
        }

        public override ICustomTypeDescriptor GetTypeDescriptor(
            Type objectType,
            object instance)
        {
            return new HlslOptionsTypeDescriptor(
                base.GetTypeDescriptor(objectType, instance));
        }

        private sealed class HlslOptionsTypeDescriptor : CustomTypeDescriptor
        {
            public HlslOptionsTypeDescriptor(ICustomTypeDescriptor parent)
                : base(parent)
            {
            }

            public override PropertyDescriptorCollection GetProperties()
            {
                return AddUnifiedSettingsMonikers(base.GetProperties());
            }

            public override PropertyDescriptorCollection GetProperties(
                Attribute[] attributes)
            {
                return AddUnifiedSettingsMonikers(base.GetProperties(attributes));
            }

            private static PropertyDescriptorCollection AddUnifiedSettingsMonikers(
                PropertyDescriptorCollection properties)
            {
                var attributeType = Type.GetType(AttributeTypeName, throwOnError: true);
                var result = properties.Cast<PropertyDescriptor>()
                    .Select(property =>
                    {
                        var moniker = GetMoniker(property.Name);
                        if (moniker == null)
                        {
                            return property;
                        }

                        var attribute = (Attribute)Activator.CreateInstance(
                            attributeType,
                            moniker);
                        return TypeDescriptor.CreateProperty(
                            typeof(HlslOptionsPage),
                            property,
                            attribute);
                    })
                    .ToArray();
                return new PropertyDescriptorCollection(result, readOnly: true);
            }

            private static string GetMoniker(string propertyName)
            {
                switch (propertyName)
                {
                    case nameof(HlslOptionsPage.FileExtensions):
                        return "hlslLsp.general.fileExtensions";
                    case nameof(HlslOptionsPage.LanguageVersion):
                        return "hlslLsp.general.languageVersion";
                    case nameof(HlslOptionsPage.DxcRuntimeDirectory):
                        return "hlslLsp.general.dxcRuntimeDirectory";
                    case nameof(HlslOptionsPage.InlayHintTypes):
                        return "hlslLsp.general.inlayHintTypes";
                    case nameof(HlslOptionsPage.InlayHintParameters):
                        return "hlslLsp.general.inlayHintParameters";
                    case nameof(HlslOptionsPage.InlayHintMatrixOrientation):
                        return "hlslLsp.general.inlayHintMatrixOrientation";
                    case nameof(HlslOptionsPage.InlayHintRegisters):
                        return "hlslLsp.general.inlayHintRegisters";
                    case nameof(HlslOptionsPage.InlayHintPackedOffsets):
                        return "hlslLsp.general.inlayHintPackedOffsets";
                    case nameof(HlslOptionsPage.InlayHintArrayStrides):
                        return "hlslLsp.general.inlayHintArrayStrides";
                    case nameof(HlslOptionsPage.InlayHintActiveVariant):
                        return "hlslLsp.general.inlayHintActiveVariant";
                    default:
                        return null;
                }
            }
        }
    }

    protected override void OnApply(PageApplyEventArgs e)
    {
        base.OnApply(e);
        if (e.ApplyBehavior == ApplyKind.Apply)
        {
            HlslBootstrapPackage.NotifyOptionsChanged();
        }
    }
}
