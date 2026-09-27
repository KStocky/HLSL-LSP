using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;

namespace HlslLsp.VisualStudio.Bootstrap;

public sealed class RuntimeCaptureStartModel
{
    public int ProtocolVersion { get; set; }

    public string SessionId { get; set; }

    public string Endpoint { get; set; }

    public string Token { get; set; }
}

public class RuntimeCaptureStatusModel
{
    public int ProtocolVersion { get; set; }

    public string SessionId { get; set; }

    public bool Active { get; set; }

    public long Accepted { get; set; }

    public long Rejected { get; set; }

    public long Overflow { get; set; }
}

public sealed class RuntimeCaptureSnapshotModel : RuntimeCaptureStatusModel
{
    public IReadOnlyList<RuntimeCaptureEntryModel> Entries { get; set; } =
        Array.Empty<RuntimeCaptureEntryModel>();
}

public sealed class RuntimeCaptureEntryModel
{
    public string Id { get; set; }

    public RuntimeCaptureInvocationModel Invocation { get; set; }

    public long Count { get; set; }

    public RuntimeCaptureReviewModel Review { get; set; }
}

public sealed class RuntimeCaptureInvocationModel
{
    public string Source { get; set; }

    public string EntryPoint { get; set; }

    public string TargetProfile { get; set; }

    public string LanguageVersion { get; set; }

    public IReadOnlyList<IReadOnlyList<string>> Defines { get; set; } =
        Array.Empty<IReadOnlyList<string>>();

    public IReadOnlyList<string> IncludeDirectories { get; set; } =
        Array.Empty<string>();

    public IReadOnlyList<IReadOnlyList<string>> VirtualMappings { get; set; } =
        Array.Empty<IReadOnlyList<string>>();

    public IReadOnlyList<string> Arguments { get; set; } =
        Array.Empty<string>();

    public string OutputMode { get; set; }

    public string Pipeline { get; set; }

    public string Stage { get; set; }
}

public sealed class RuntimeCaptureReviewModel
{
    public bool Eligible { get; set; }

    public bool RequiresConfirmation { get; set; }

    public ConfigurationSelectionModel Selection { get; set; }

    public JObject Settings { get; set; }

    public JObject FileGroup { get; set; }

    public IReadOnlyList<string> WarningCodes { get; set; } = Array.Empty<string>();

    public IReadOnlyList<string> Warnings { get; set; } = Array.Empty<string>();
}

public sealed class RuntimeCaptureVariantModel
{
    public string EntryId { get; set; }

    public string Name { get; set; }
}

public sealed class RuntimeCapturePipelineModel
{
    public string Name { get; set; }

    public string Source { get; set; }

    public IReadOnlyDictionary<string, string> Stages { get; set; }
}

public sealed class RuntimeCapturePreviewModel : ConfigurationAuthoringModel
{
    public string SessionId { get; set; }

    public IReadOnlyList<string> SelectedEntryIds { get; set; } =
        Array.Empty<string>();
}

internal sealed class RuntimeCaptureSelection
{
    private readonly IReadOnlyList<RuntimeCaptureEntryModel> entries;
    private readonly HashSet<string> selected = new(StringComparer.Ordinal);

    internal RuntimeCaptureSelection(RuntimeCaptureSnapshotModel snapshot)
    {
        entries = snapshot?.Entries ?? Array.Empty<RuntimeCaptureEntryModel>();
    }

    internal IReadOnlyList<RuntimeCaptureEntryModel> Entries => entries;

    internal IReadOnlyList<RuntimeCaptureEntryModel> Selected =>
        entries.Where(entry => entry != null && selected.Contains(entry.Id)).ToArray();

    internal bool CanSelect(RuntimeCaptureEntryModel entry) =>
        entry != null && !string.IsNullOrEmpty(entry.Id) &&
        entry.Count > 0 && entry.Review?.Eligible == true;

    internal void SetSelected(RuntimeCaptureEntryModel entry, bool value)
    {
        if (!entries.Contains(entry) || !CanSelect(entry))
        {
            throw new ArgumentException("Entry cannot be selected for capture review.", nameof(entry));
        }
        if (value)
        {
            selected.Add(entry.Id);
        }
        else
        {
            selected.Remove(entry.Id);
        }
    }
}

public static class RuntimeCaptureBridge
{
    private static Func<CancellationToken, Task<RuntimeCaptureStartModel>> start;
    private static Func<Uri, CancellationToken, Task<RuntimeCaptureSnapshotModel>> snapshot;
    private static Func<string, CancellationToken, Task> stop;
    private static Func<Uri, string, IReadOnlyList<string>, string, long?, string,
        IReadOnlyList<RuntimeCaptureVariantModel>,
        IReadOnlyList<RuntimeCapturePipelineModel>,
        CancellationToken, Task<RuntimeCapturePreviewModel>> preview;

    public static void Register(
        Func<CancellationToken, Task<RuntimeCaptureStartModel>> startHandler,
        Func<Uri, CancellationToken, Task<RuntimeCaptureSnapshotModel>> snapshotHandler,
        Func<string, CancellationToken, Task> stopHandler,
        Func<Uri, string, IReadOnlyList<string>, string, long?, string,
            IReadOnlyList<RuntimeCaptureVariantModel>,
            IReadOnlyList<RuntimeCapturePipelineModel>,
            CancellationToken, Task<RuntimeCapturePreviewModel>> previewHandler)
    {
        Volatile.Write(ref start, startHandler ?? throw new ArgumentNullException(nameof(startHandler)));
        Volatile.Write(ref snapshot, snapshotHandler ?? throw new ArgumentNullException(nameof(snapshotHandler)));
        Volatile.Write(ref stop, stopHandler ?? throw new ArgumentNullException(nameof(stopHandler)));
        Volatile.Write(ref preview, previewHandler ?? throw new ArgumentNullException(nameof(previewHandler)));
    }

    public static bool IsAvailable => Volatile.Read(ref start) != null;

    public static Task<RuntimeCaptureStartModel> StartAsync(CancellationToken cancellationToken)
        => Volatile.Read(ref start)?.Invoke(cancellationToken)
            ?? throw new InvalidOperationException("The HLSL capture client is not available.");

    public static Task<RuntimeCaptureSnapshotModel> SnapshotAsync(
        Uri workspaceFolder, CancellationToken cancellationToken)
        => Volatile.Read(ref snapshot)?.Invoke(workspaceFolder, cancellationToken)
            ?? throw new InvalidOperationException("The HLSL capture client is not available.");

    public static Task StopAsync(string token, CancellationToken cancellationToken)
        => Volatile.Read(ref stop)?.Invoke(token, cancellationToken)
            ?? throw new InvalidOperationException("The HLSL capture client is not available.");

    public static Task<RuntimeCapturePreviewModel> PreviewAsync(
        Uri workspaceFolder,
        string sessionId,
        IReadOnlyList<string> selectedEntryIds,
        string existingContent,
        long? existingVersion,
        string existingContentHash,
        IReadOnlyList<RuntimeCaptureVariantModel> variants,
        IReadOnlyList<RuntimeCapturePipelineModel> pipelines,
        CancellationToken cancellationToken)
        => Volatile.Read(ref preview)?.Invoke(
            workspaceFolder, sessionId, selectedEntryIds,
            existingContent, existingVersion, existingContentHash,
            variants, pipelines, cancellationToken)
            ?? throw new InvalidOperationException("The HLSL capture client is not available.");
}
