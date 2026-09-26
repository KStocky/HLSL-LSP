using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace HlslLsp.VisualStudio.Bootstrap;

public enum HlslLifecycleState
{
    NotActivated,
    Activating,
    Starting,
    Connected,
    Restarting,
    Disconnected,
    Failed,
}

public sealed class HlslDxcRuntimeModel
{
    public string Source { get; set; }

    public string Directory { get; set; }

    public string LibraryPath { get; set; }

    public string Version { get; set; }

    public bool RequiresRestart { get; set; }

    public string Error { get; set; }
}

public sealed class HlslStatusSnapshot
{
    public HlslLifecycleState Lifecycle { get; set; }

    public HlslDxcRuntimeModel Runtime { get; set; }

    public EffectiveShaderContextModel Context { get; set; }

    public string LastFailure { get; set; }
}

internal static class HlslStatusDisplay
{
    private static readonly Regex AbsoluteWindowsPath = new(
        @"(?i)(?:[a-z]:\\|\\\\)[^""'\r\n]+",
        RegexOptions.Compiled);

    internal static string Lifecycle(HlslLifecycleState state)
    {
        switch (state)
        {
            case HlslLifecycleState.NotActivated:
                return "Not activated";
            case HlslLifecycleState.Activating:
                return "Activating";
            case HlslLifecycleState.Starting:
                return "Starting";
            case HlslLifecycleState.Connected:
                return "Connected";
            case HlslLifecycleState.Restarting:
                return "Restarting";
            case HlslLifecycleState.Disconnected:
                return "Disconnected";
            case HlslLifecycleState.Failed:
                return "Attention required";
            default:
                throw new ArgumentOutOfRangeException(nameof(state));
        }
    }

    internal static string Runtime(HlslDxcRuntimeModel runtime)
    {
        if (runtime == null)
        {
            return "DXC runtime: Not available";
        }
        var version = EffectiveShaderContextDisplay.Value(runtime.Version);
        var source = string.IsNullOrWhiteSpace(runtime.Source)
            ? "unknown source"
            : runtime.Source;
        return $"DXC {version} ({source})";
    }

    internal static string Diagnostics(HlslStatusSnapshot snapshot)
    {
        snapshot = snapshot ?? new HlslStatusSnapshot();
        var builder = new StringBuilder();
        builder.AppendLine("HLSL-LSP diagnostics");
        builder.Append("Lifecycle: ").AppendLine(Lifecycle(snapshot.Lifecycle));
        builder.Append("Runtime: ").AppendLine(Runtime(snapshot.Runtime));
        if (snapshot.Runtime?.RequiresRestart == true)
        {
            builder.AppendLine("Runtime restart pending: Yes");
        }
        if (!string.IsNullOrWhiteSpace(snapshot.Runtime?.Error))
        {
            builder.Append("Runtime error: ")
                .AppendLine(RedactAbsolutePaths(snapshot.Runtime.Error));
        }

        var context = snapshot.Context;
        builder.Append("Shader: ")
            .AppendLine(context == null
                ? EffectiveShaderContextDisplay.NotConfigured
                : SafeFileName(context.File));
        builder.Append("Variant: ")
            .AppendLine(EffectiveShaderContextDisplay.Variant(context?.ActiveVariant));
        builder.Append("Entry point: ")
            .AppendLine(EffectiveShaderContextDisplay.Value(context?.EntryPoint));
        builder.Append("Target profile: ")
            .AppendLine(EffectiveShaderContextDisplay.Value(context?.TargetProfile));

        var origins = SafeOriginSummary(context);
        if (!string.IsNullOrEmpty(origins))
        {
            builder.Append("Configuration provenance: ").AppendLine(origins);
        }
        if (!string.IsNullOrWhiteSpace(snapshot.LastFailure))
        {
            builder.Append("Last failure: ")
                .AppendLine(RedactAbsolutePaths(snapshot.LastFailure));
        }
        return builder.ToString().TrimEnd();
    }

    internal static string SafeOriginSummary(EffectiveShaderContextModel context)
    {
        if (context?.Origins == null)
        {
            return string.Empty;
        }
        var values = new List<string>();
        AddOrigin(values, "Variant", context.Origins.Variant);
        AddOrigin(values, "Entry point", context.Origins.EntryPoint);
        AddOrigin(values, "Target profile", context.Origins.TargetProfile);
        return string.Join("; ", values);
    }

    internal static string SafeFileName(string value)
    {
        if (string.IsNullOrWhiteSpace(value))
        {
            return EffectiveShaderContextDisplay.NotConfigured;
        }
        try
        {
            return Path.GetFileName(value);
        }
        catch (ArgumentException)
        {
            return value;
        }
    }

    internal static string RedactAbsolutePaths(string value)
    {
        if (string.IsNullOrEmpty(value))
        {
            return value;
        }
        return AbsoluteWindowsPath.Replace(
            value,
            match =>
            {
                var path = match.Value.TrimEnd('.', ',', ';', ')', ']');
                var suffix = match.Value.Substring(path.Length);
                return "<path>\\" + SafeFileName(path) + suffix;
            });
    }

    private static void AddOrigin(
        ICollection<string> values,
        string label,
        EffectiveContextOriginModel origin)
    {
        if (!string.IsNullOrWhiteSpace(origin?.Label))
        {
            values.Add($"{label} — {RedactOriginLabel(origin.Label)}");
        }
    }

    private static string RedactOriginLabel(string label)
    {
        if (Uri.TryCreate(label, UriKind.Absolute, out var uri) && uri.IsFile)
        {
            return SafeFileName(uri.LocalPath);
        }
        return Path.IsPathRooted(label) ? SafeFileName(label) : label;
    }
}

public static class HlslStatusBridge
{
    private const int MaximumOutputLines = 200;
    private static readonly object Gate = new();
    private static readonly Queue<string> OutputLines = new();
    private static HlslStatusSnapshot snapshot = new()
    {
        Lifecycle = HlslLifecycleState.NotActivated,
    };
    private static Func<CancellationToken, Task> restart;
    private static Func<CancellationToken, Task<HlslDxcRuntimeModel>> runtimeRequest;

    public static event Action Changed;

    public static event Action<string> OutputAppended;

    public static HlslStatusSnapshot Snapshot
    {
        get
        {
            lock (Gate)
            {
                return Clone(snapshot);
            }
        }
    }

    public static void RegisterRestart(Func<CancellationToken, Task> handler)
    {
        lock (Gate)
        {
            restart = handler ?? throw new ArgumentNullException(nameof(handler));
        }
    }

    public static void RegisterRuntimeRequest(
        Func<CancellationToken, Task<HlslDxcRuntimeModel>> handler)
    {
        lock (Gate)
        {
            runtimeRequest = handler ?? throw new ArgumentNullException(nameof(handler));
        }
    }

    public static Task RestartAsync(CancellationToken cancellationToken)
    {
        Func<CancellationToken, Task> handler;
        lock (Gate)
        {
            handler = restart;
        }
        return handler == null
            ? Task.FromException(
                new InvalidOperationException("The HLSL language client is not active."))
            : handler(cancellationToken);
    }

    public static Task<HlslDxcRuntimeModel> RequestRuntimeAsync(
        CancellationToken cancellationToken)
    {
        Func<CancellationToken, Task<HlslDxcRuntimeModel>> handler;
        lock (Gate)
        {
            handler = runtimeRequest;
        }
        return handler == null
            ? Task.FromResult<HlslDxcRuntimeModel>(null)
            : handler(cancellationToken);
    }

    public static void ReportLifecycle(HlslLifecycleState lifecycle)
    {
        lock (Gate)
        {
            if (lifecycle == HlslLifecycleState.Disconnected &&
                snapshot.Lifecycle == HlslLifecycleState.Restarting)
            {
                return;
            }
            snapshot.Lifecycle = lifecycle;
            if (lifecycle == HlslLifecycleState.Connected)
            {
                snapshot.LastFailure = null;
            }
        }
        Changed?.Invoke();
    }

    public static void ReportFailure(string failure)
    {
        if (string.IsNullOrWhiteSpace(failure))
        {
            return;
        }
        lock (Gate)
        {
            if (snapshot.Lifecycle != HlslLifecycleState.Connected)
            {
                snapshot.Lifecycle = HlslLifecycleState.Failed;
            }
            snapshot.LastFailure = failure.Trim();
        }
        AppendOutput("ERROR: " + failure.Trim());
        Changed?.Invoke();
    }

    public static void UpdateDetails(
        HlslDxcRuntimeModel runtime,
        EffectiveShaderContextModel context)
    {
        lock (Gate)
        {
            snapshot.Runtime = runtime;
            snapshot.Context = context;
        }
        Changed?.Invoke();
    }

    public static void AppendOutput(string line)
    {
        if (string.IsNullOrWhiteSpace(line))
        {
            return;
        }
        var value = line.TrimEnd();
        lock (Gate)
        {
            OutputLines.Enqueue(value);
            while (OutputLines.Count > MaximumOutputLines)
            {
                OutputLines.Dequeue();
            }
        }
        OutputAppended?.Invoke(value);
    }

    public static string OutputSnapshot()
    {
        lock (Gate)
        {
            return string.Join(Environment.NewLine, OutputLines);
        }
    }

    private static HlslStatusSnapshot Clone(HlslStatusSnapshot value)
        => new()
        {
            Lifecycle = value.Lifecycle,
            Runtime = value.Runtime,
            Context = value.Context,
            LastFailure = value.LastFailure,
        };
}
