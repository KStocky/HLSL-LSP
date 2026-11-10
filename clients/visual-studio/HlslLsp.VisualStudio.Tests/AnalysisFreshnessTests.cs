using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using HlslLsp.VisualStudio;
using HlslLsp.VisualStudio.Bootstrap;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class AnalysisFreshnessTests
{
    [Fact]
    public void Reducer_UsesSharedDeterministicStatesAndCauses()
    {
        var refreshing = AnalysisFreshnessReducer.Reduce(
            AnalysisFreshnessReducer.Initial,
            new AnalysisFreshnessEvent(
                AnalysisFreshnessEventKind.RefreshStarted,
                AnalysisFreshnessCause.ManualRefresh));
        Assert.Equal(AnalysisFreshnessStatus.Refreshing, refreshing.Status);
        Assert.Equal(AnalysisFreshnessCause.ManualRefresh, refreshing.Cause);
        Assert.Equal("Refreshing · Manual refresh", AnalysisFreshnessReducer.Text(refreshing));

        var current = AnalysisFreshnessReducer.Reduce(
            refreshing,
            new AnalysisFreshnessEvent(
                AnalysisFreshnessEventKind.RefreshSucceeded));
        Assert.Equal(AnalysisFreshnessStatus.Current, current.Status);
        Assert.Equal("Current", AnalysisFreshnessReducer.Text(current));

        var failed = AnalysisFreshnessReducer.Reduce(
            refreshing,
            new AnalysisFreshnessEvent(
                AnalysisFreshnessEventKind.RefreshFailed));
        Assert.Equal(AnalysisFreshnessStatus.RefreshFailed, failed.Status);
        Assert.Equal(AnalysisFreshnessCause.ManualRefresh, failed.Cause);
        Assert.Equal("Refresh failed · Manual refresh", AnalysisFreshnessReducer.Text(failed));

        var disconnected = AnalysisFreshnessReducer.Reduce(
            current,
            new AnalysisFreshnessEvent(
                AnalysisFreshnessEventKind.Invalidated,
                AnalysisFreshnessCause.DisconnectedServer));
        Assert.Equal(AnalysisFreshnessStatus.Stale, disconnected.Status);
        Assert.Equal("Stale · Disconnected server", AnalysisFreshnessReducer.Text(disconnected));
    }

    [Fact]
    public void Reducer_CoalescedInvalidationDoesNotFlickerOutOfRefreshing()
    {
        var refreshing = new AnalysisFreshnessState(
            AnalysisFreshnessStatus.Refreshing,
            AnalysisFreshnessCause.SourceEdit);
        var coalesced = AnalysisFreshnessReducer.Reduce(
            refreshing,
            new AnalysisFreshnessEvent(
                AnalysisFreshnessEventKind.Invalidated,
                AnalysisFreshnessCause.SourceEdit,
                true));
        Assert.Equal(AnalysisFreshnessStatus.Refreshing, coalesced.Status);
        Assert.Equal(AnalysisFreshnessCause.SourceEdit, coalesced.Cause);
    }

    [Fact]
    public void ManualRefreshBridge_DispatchesEveryViewWithoutDoingRpcInline()
    {
        var requested = new List<AnalysisViewKind>();
        AnalysisFreshnessBridge.Register(requested.Add);

        AnalysisFreshnessBridge.RequestRefresh(AnalysisViewKind.MemoryLayout);
        AnalysisFreshnessBridge.RequestRefresh(AnalysisViewKind.CompilationInfo);
        AnalysisFreshnessBridge.RequestRefresh(AnalysisViewKind.ResourceBindings);
        AnalysisFreshnessBridge.RequestRefresh(AnalysisViewKind.PreprocessorExplorer);
        AnalysisFreshnessBridge.RequestRefresh(AnalysisViewKind.EntryPointDataFlow);
        AnalysisFreshnessBridge.RequestRefresh(AnalysisViewKind.ComputeVisualization);
        AnalysisFreshnessBridge.RequestRefresh(AnalysisViewKind.CallHierarchy);

        Assert.Equal(
            new[]
            {
                AnalysisViewKind.MemoryLayout,
                AnalysisViewKind.CompilationInfo,
                AnalysisViewKind.ResourceBindings,
                AnalysisViewKind.PreprocessorExplorer,
                AnalysisViewKind.EntryPointDataFlow,
                AnalysisViewKind.ComputeVisualization,
                AnalysisViewKind.CallHierarchy,
            },
            requested);
    }

    [Fact]
    public void KnownEdit_InvalidatesInFlightMemoryResultAndDefersReplacement()
    {
        var gate = new MemoryLayoutRefreshGate();
        var request = gate.EnterExplicitRequest();

        gate.InvalidateForRefresh();

        Assert.False(gate.IsCurrent(request));
        Assert.True(gate.ExitExplicitRequest());
    }

    [Fact]
    public void KnownEdit_IsReplayedAfterExplicitGatedViewsFinish()
    {
        var gate = new EntryPointDataFlowRefreshGate();
        gate.EnterExplicitRequest();

        gate.RecordRefreshNeeded();

        Assert.True(gate.ExitExplicitRequest());
    }

    [Fact]
    public void SimpleViewGate_PreservesKnownCauseForDeferredReplacement()
    {
        var gate = new AnalysisRefreshGate();
        gate.EnterExplicitRequest();

        Assert.False(
            gate.TryBeginBackgroundRefresh(
                AnalysisFreshnessCause.VariantChange));
        Assert.True(gate.ExitExplicitRequest(out var cause));
        Assert.Equal(AnalysisFreshnessCause.VariantChange, cause);
    }

    [Fact]
    public async Task LanguageClient_ReportsConnectedAndDisconnectedStates()
    {
        var states = new List<bool>();
        var client = new HlslLanguageClient(
            "2021",
            string.Empty,
            string.Empty,
            (_, _) => Task.CompletedTask,
            _ => Task.CompletedTask,
            onConnectionStateChanged: states.Add);

        await client.OnServerInitializedAsync();
        await client.StopServerAsync();

        Assert.Equal(new[] { true, false }, states);
    }

    [Fact]
    public void LanguageClient_IgnoresExitFromReplacedProcess()
    {
        using var current = new Process();
        using var replaced = new Process();

        Assert.True(HlslLanguageClient.IsCurrentServerProcess(current, current));
        Assert.False(HlslLanguageClient.IsCurrentServerProcess(current, replaced));
        Assert.False(HlslLanguageClient.IsCurrentServerProcess(null, replaced));
    }

    [Fact]
    public async Task RuntimeRestart_WaitsForHostStopBeforeStartingAgain()
    {
        var client = CreateLifecycleClient();
        var stopEntered = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var finishStop = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var startEntered = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        client.StopAsync += async (_, _) =>
        {
            stopEntered.TrySetResult(true);
            await finishStop.Task;
        };
        client.StartAsync += (_, _) =>
        {
            startEntered.TrySetResult(true);
            return Task.CompletedTask;
        };

        var restart = client.RestartWithRuntimeAsync("202x", @"C:\runtime");
        try
        {
            await stopEntered.Task;
            await Task.WhenAny(startEntered.Task, Task.Delay(1000));
            Assert.False(startEntered.Task.IsCompleted);
            Assert.False(restart.IsCompleted);
        }
        finally
        {
            finishStop.TrySetResult(true);
            await restart;
        }
        Assert.True(startEntered.Task.IsCompleted);
    }

    [Fact]
    public async Task RuntimeRestart_SerializesConcurrentRequestsAndTheirSettings()
    {
        var client = CreateLifecycleClient();
        var finishFirstStop = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var stopCount = 0;
        var startedRuntimes = new List<string>();
        client.StopAsync += (_, _) =>
            Interlocked.Increment(ref stopCount) == 1
                ? finishFirstStop.Task
                : Task.CompletedTask;
        client.StartAsync += (_, _) =>
        {
            startedRuntimes.Add(client.RuntimeDirectory);
            return Task.CompletedTask;
        };

        var first = client.RestartWithRuntimeAsync("2021", @"C:\first-runtime");
        var second = client.RestartWithRuntimeAsync("202x", @"C:\second-runtime");
        try
        {
            Assert.Equal(1, Volatile.Read(ref stopCount));
            Assert.Empty(startedRuntimes);
        }
        finally
        {
            finishFirstStop.TrySetResult(true);
            await Task.WhenAll(first, second);
        }
        Assert.Equal(new[] { @"C:\first-runtime", @"C:\second-runtime" }, startedRuntimes);
    }

    [Fact]
    public async Task RuntimeRestart_WaitsForInitialHostStartup()
    {
        var client = CreateLifecycleClient();
        var finishStartup = new TaskCompletionSource<bool>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        var events = new List<string>();
        client.StartAsync += (_, _) =>
        {
            events.Add("start");
            return events.Count == 1 ? finishStartup.Task : Task.CompletedTask;
        };
        client.StopAsync += (_, _) =>
        {
            events.Add("stop");
            return Task.CompletedTask;
        };

        var startup = client.OnLoadedAsync();
        var restart = client.RestartWithRuntimeAsync("202x", @"C:\runtime");
        try
        {
            Assert.Equal(new[] { "start" }, events);
        }
        finally
        {
            finishStartup.TrySetResult(true);
            await Task.WhenAll(startup, restart);
        }
        Assert.Equal(new[] { "start", "stop", "start" }, events);
    }

    [Fact]
    public async Task RuntimeRestart_FailedStopDoesNotStartAndAllowsRetry()
    {
        var client = CreateLifecycleClient();
        var stopCount = 0;
        var startCount = 0;
        client.StopAsync += (_, _) =>
            Interlocked.Increment(ref stopCount) == 1
                ? Task.FromException(new InvalidOperationException("Host stop failed"))
                : Task.CompletedTask;
        client.StartAsync += (_, _) =>
        {
            ++startCount;
            return Task.CompletedTask;
        };

        await Assert.ThrowsAsync<AggregateException>(
            () => client.RestartWithRuntimeAsync("202x", @"C:\runtime"));
        Assert.Equal(0, startCount);
        Assert.Equal(string.Empty, client.RuntimeDirectory);
        await client.RestartWithRuntimeAsync("202x", @"C:\runtime");
        Assert.Equal(1, startCount);
        Assert.Equal(@"C:\runtime", client.RuntimeDirectory);
    }

    [Fact]
    public async Task RuntimeStop_NotifiesHostWhileTransportProcessIsStillAlive()
    {
        var client = CreateLifecycleClient();
        using var process = StartWaitingProcess();
        using var observer = Process.GetProcessById(process.Id);
        SetServerProcess(client, process);
        var transportWasAlive = false;
        client.StopAsync += (_, _) =>
        {
            transportWasAlive = !observer.HasExited;
            return Task.CompletedTask;
        };
        try
        {
            await client.StopServerAsync();
            Assert.True(transportWasAlive);
            Assert.True(observer.WaitForExit(5000));
        }
        finally
        {
            if (!observer.HasExited)
            {
                observer.Kill();
                observer.WaitForExit();
            }
        }
    }

    [Fact]
    public async Task RuntimeStop_FailedHostStopDoesNotKillTheActiveProcess()
    {
        var client = CreateLifecycleClient();
        using var process = StartWaitingProcess();
        using var observer = Process.GetProcessById(process.Id);
        SetServerProcess(client, process);
        client.StopAsync += (_, _) =>
            Task.FromException(new InvalidOperationException("Host stop failed"));
        try
        {
            var error = await Assert.ThrowsAsync<AggregateException>(
                client.StopServerAsync);
            Assert.Equal("Host stop failed", Assert.Single(error.Flatten().InnerExceptions).Message);
            Assert.False(observer.HasExited);
        }
        finally
        {
            if (!observer.HasExited)
            {
                observer.Kill();
                observer.WaitForExit();
            }
        }
    }

    [Fact]
    public async Task RuntimeActivation_IsBlockedWhileHostStopIsInProgress()
    {
        var client = CreateLifecycleClient();
        client.StopAsync += async (_, _) =>
        {
            var error = await Assert.ThrowsAsync<InvalidOperationException>(
                () => client.ActivateAsync(CancellationToken.None));
            Assert.Contains("being stopped", error.Message);
        };

        await client.StopServerAsync();
    }

    [Fact]
    public async Task RuntimeActivation_RejectsAnAlreadyOwnedProcess()
    {
        var client = CreateLifecycleClient();
        using var process = StartWaitingProcess();
        using var observer = Process.GetProcessById(process.Id);
        SetServerProcess(client, process);
        try
        {
            var error = await Assert.ThrowsAsync<InvalidOperationException>(
                () => client.ActivateAsync(CancellationToken.None));
            Assert.Contains("already", error.Message);
            Assert.False(observer.HasExited);
        }
        finally
        {
            if (!observer.HasExited)
            {
                observer.Kill();
                observer.WaitForExit();
            }
        }
    }

    private static HlslLanguageClient CreateLifecycleClient() => new(
        "2021", string.Empty, string.Empty,
        (_, _) => Task.CompletedTask, _ => Task.CompletedTask);

    private static Process StartWaitingProcess() =>
        Process.Start(new ProcessStartInfo
        {
            FileName = Environment.GetEnvironmentVariable("ComSpec") ?? "cmd.exe",
            Arguments = "/d /q /c \"set /p HLSL_LSP_TEST_WAIT=\"",
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        });

    private static void SetServerProcess(HlslLanguageClient client, Process process) =>
        typeof(HlslLanguageClient)
            .GetField("serverProcess", BindingFlags.Instance | BindingFlags.NonPublic)
            .SetValue(client, process);

    [Fact]
    public void NavigationMismatch_DemotesCallHierarchyFromRefreshingToStale()
    {
        var result = CallHierarchyRefreshFreshness.AfterNavigationMismatch(
            new AnalysisFreshnessState(
                AnalysisFreshnessStatus.Refreshing,
                AnalysisFreshnessCause.SourceEdit),
            AnalysisFreshnessCause.SourceEdit);

        Assert.Equal(AnalysisFreshnessStatus.Stale, result.Status);
        Assert.Equal(AnalysisFreshnessCause.SourceEdit, result.Cause);
    }

    [Fact]
    public async Task TrackedRefreshCancellation_BoundsHungRequestsWithoutCancellingAmbientToken()
    {
        using var ambient = new CancellationTokenSource();
        using var request = AnalysisRefreshCancellation.CreateLinked(
            ambient.Token,
            TimeSpan.FromMilliseconds(20));

        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => Task.Delay(Timeout.InfiniteTimeSpan, request.Token));
        Assert.False(ambient.IsCancellationRequested);
    }

    [Fact]
    public void CausePolicy_UsesOrderIndependentDeterministicPrecedence()
    {
        Assert.Equal(
            AnalysisFreshnessCause.DisconnectedServer,
            AnalysisFreshnessCausePolicy.Coalesce(
                AnalysisFreshnessCause.SourceEdit,
                AnalysisFreshnessCause.DisconnectedServer));
        Assert.Equal(
            AnalysisFreshnessCause.DisconnectedServer,
            AnalysisFreshnessCausePolicy.Coalesce(
                AnalysisFreshnessCause.DisconnectedServer,
                AnalysisFreshnessCause.SourceEdit));
        Assert.Equal(
            AnalysisFreshnessCause.VariantChange,
            AnalysisFreshnessCausePolicy.Coalesce(
                AnalysisFreshnessCause.ConfigurationChange,
                AnalysisFreshnessCause.VariantChange));
    }

    [Fact]
    public void MemoryGate_CoalescesAndReturnsHighestPriorityCause()
    {
        var gate = new MemoryLayoutRefreshGate();
        gate.EnterExplicitRequest();

        gate.InvalidateForRefresh(AnalysisFreshnessCause.SourceEdit);
        Assert.False(
            gate.TryBeginBackgroundRefresh(
                AnalysisFreshnessCause.VariantChange,
                out _));
        gate.InvalidateForRefresh(AnalysisFreshnessCause.DisconnectedServer);

        Assert.True(gate.ExitExplicitRequest(out var cause));
        Assert.Equal(AnalysisFreshnessCause.DisconnectedServer, cause);
    }

    [Fact]
    public void DisconnectInvalidation_DoesNotLeaveRefreshPendingOrRefreshing()
    {
        var gate = new MemoryLayoutRefreshGate();
        var request = gate.EnterExplicitRequest();

        gate.InvalidateForRefresh(
            AnalysisFreshnessCause.DisconnectedServer,
            false);
        var freshness = AnalysisFreshnessReducer.Reduce(
            new AnalysisFreshnessState(
                AnalysisFreshnessStatus.Refreshing,
                AnalysisFreshnessCause.SourceEdit),
            new AnalysisFreshnessEvent(
                AnalysisFreshnessEventKind.Invalidated,
                AnalysisFreshnessCause.DisconnectedServer,
                false));

        Assert.False(gate.IsCurrent(request));
        Assert.False(gate.ExitExplicitRequest(out _));
        Assert.Equal(AnalysisFreshnessStatus.Stale, freshness.Status);
        Assert.Equal(
            AnalysisFreshnessCause.DisconnectedServer,
            freshness.Cause);
    }

    [Fact]
    public void SharedExplicitGate_CoalescesCauseForEntryComputeAndCallHierarchy()
    {
        var gate = new EntryPointDataFlowRefreshGate();
        gate.EnterExplicitRequest();

        gate.RecordRefreshNeeded(AnalysisFreshnessCause.ManualRefresh);
        Assert.False(
            gate.TryBeginBackgroundRefresh(
                AnalysisFreshnessCause.ConfigurationChange));
        gate.RecordRefreshNeeded(AnalysisFreshnessCause.VariantChange);

        Assert.True(gate.ExitExplicitRequest(out var cause));
        Assert.Equal(AnalysisFreshnessCause.VariantChange, cause);
    }

    [Theory]
    [InlineData(@"C:\project\shader.hlsl", "Source")]
    [InlineData(@"C:\project\include.hlsli", "Source")]
    [InlineData(@"C:\project\include.customh", "Source")]
    [InlineData(@"C:\project\shadertoolsconfig.json", "Configuration")]
    [InlineData(@"C:\project\notes.txt", "None")]
    [InlineData(@"C:\project\Program.cs", "None")]
    public void SavedAnalysisPolicy_OnlyInvalidatesRelevantSourcesAndConfiguration(
        string path,
        string expected)
    {
        Assert.Equal(
            expected,
            HlslSavedAnalysisPolicy.Classify(
                path,
                new[] { ".customh", ".usf" }).ToString());
    }

    [Fact]
    public void OptionsPolicy_InlayOnlyChangeDoesNotAffectAnalysis()
    {
        var previous = Options(inlayTypes: true);
        var current = Options(inlayTypes: false);

        var change = HlslOptionsAnalysisPolicy.Classify(previous, current);

        Assert.True(change.InlayHintsChanged);
        Assert.False(change.AnalysisAffected);
    }

    [Fact]
    public void OptionsPolicy_ClassifiesLanguageExtensionsAndRuntimeAsAnalysisChanges()
    {
        var previous = Options();

        var language = HlslOptionsAnalysisPolicy.Classify(
            previous,
            Options(languageVersion: "202x"));
        var extensions = HlslOptionsAnalysisPolicy.Classify(
            previous,
            Options(fileExtensions: ".hlsl;.hlsli;.usf;.customh"));
        var runtime = HlslOptionsAnalysisPolicy.Classify(
            previous,
            Options(dxcRuntimeDirectory: @"C:\DXC"));

        Assert.True(language.LanguageVersionChanged);
        Assert.True(language.AnalysisAffected);
        Assert.True(extensions.FileExtensionsChanged);
        Assert.True(extensions.AnalysisAffected);
        Assert.True(runtime.RuntimeChanged);
        Assert.True(runtime.AnalysisAffected);
    }

    [Fact]
    public void OptionsPolicy_TreatsExtensionOrderAndRuntimeCaseAsEquivalent()
    {
        var change = HlslOptionsAnalysisPolicy.Classify(
            Options(
                fileExtensions: ".hlsl;.customh;.hlsli",
                dxcRuntimeDirectory: @"C:\DXC"),
            Options(
                fileExtensions: ".HLSLI;.HLSL;.CUSTOMH",
                dxcRuntimeDirectory: @"c:\dxc "));

        Assert.False(change.AnalysisAffected);
    }

    [Fact]
    public void CrossDocumentSave_RefreshesOnlyDependencyAwareViews()
    {
        var tracked = new Uri("file:///project/root.hlsl");
        const string saved = @"C:\project\other.hlsl";
        var extensions = new[] { ".hlsl", ".hlsli", ".customh" };

        Assert.True(
            AnalysisSaveRefreshPolicy.ShouldRefresh(
                AnalysisViewKind.MemoryLayout,
                tracked,
                saved,
                extensions));
        Assert.False(
            AnalysisSaveRefreshPolicy.ShouldRefresh(
                AnalysisViewKind.CompilationInfo,
                tracked,
                saved,
                extensions));
        Assert.False(
            AnalysisSaveRefreshPolicy.ShouldRefresh(
                AnalysisViewKind.ResourceBindings,
                tracked,
                saved,
                extensions));
        Assert.False(
            AnalysisSaveRefreshPolicy.ShouldRefresh(
                AnalysisViewKind.PreprocessorExplorer,
                tracked,
                saved,
                extensions));
        Assert.True(
            AnalysisSaveRefreshPolicy.ShouldRefresh(
                AnalysisViewKind.EntryPointDataFlow,
                tracked,
                saved,
                extensions));
        Assert.True(
            AnalysisSaveRefreshPolicy.ShouldRefresh(
                AnalysisViewKind.ComputeVisualization,
                tracked,
                saved,
                extensions));
        Assert.True(
            AnalysisSaveRefreshPolicy.ShouldRefresh(
                AnalysisViewKind.CallHierarchy,
                tracked,
                saved,
                extensions));
    }

    [Fact]
    public void IncludeSave_RefreshesDependencyAwareViewsIncludingMemoryLayout()
    {
        var tracked = new Uri("file:///project/root.hlsl");

        Assert.True(
            AnalysisSaveRefreshPolicy.ShouldRefresh(
                AnalysisViewKind.MemoryLayout,
                tracked,
                @"C:\project\include.customh",
                new[] { ".customh" }));
        Assert.True(
            AnalysisSaveRefreshPolicy.ShouldRefresh(
                AnalysisViewKind.CallHierarchy,
                tracked,
                @"C:\project\include.hlsli",
                Array.Empty<string>()));
    }

    [Fact]
    public void UnrelatedSave_DoesNotAdvanceMemoryGeneration()
    {
        var gate = new MemoryLayoutRefreshGate();
        Assert.True(
            gate.TryBeginBackgroundRefresh(
                AnalysisFreshnessCause.SourceEdit,
                out var generation));

        var shouldRefresh = AnalysisSaveRefreshPolicy.ShouldRefresh(
            AnalysisViewKind.CompilationInfo,
            new Uri("file:///project/root.hlsl"),
            @"C:\project\other.hlsl",
            new[] { ".hlsl" });

        Assert.False(shouldRefresh);
        Assert.True(gate.IsCurrent(generation));
    }

    private static HlslOptionsSnapshot Options(
        string fileExtensions = ".hlsl;.hlsli;.usf",
        string languageVersion = "2021",
        string dxcRuntimeDirectory = "",
        bool inlayTypes = true)
        => new(
            fileExtensions,
            languageVersion,
            dxcRuntimeDirectory,
            new InlayHintOptionsSnapshot(
                inlayTypes,
                true,
                false,
                false,
                false,
                false,
                true));
}
