using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json;

namespace HlslLsp.VisualStudio.Bootstrap;

public sealed class ConfigurationAuthoringRequestModel
{
    public int ProtocolVersion { get; set; } = 1;

    public ConfigurationWorkspaceFolderModel WorkspaceFolder { get; set; }

    [JsonProperty(NullValueHandling = NullValueHandling.Ignore)]
    public ConfigurationExistingModel ExistingConfiguration { get; set; }

    [JsonProperty(NullValueHandling = NullValueHandling.Ignore)]
    public IReadOnlyList<ConfigurationSelectionModel> Selections { get; set; }
}

public sealed class ConfigurationWorkspaceFolderModel
{
    public string Uri { get; set; }
}

public sealed class ConfigurationExistingModel
{
    [JsonProperty(NullValueHandling = NullValueHandling.Ignore)]
    public string Content { get; set; }

    [JsonProperty(NullValueHandling = NullValueHandling.Ignore)]
    public long? Version { get; set; }

    [JsonProperty(NullValueHandling = NullValueHandling.Ignore)]
    public string ContentHash { get; set; }
}

public sealed class ConfigurationSelectionModel
{
    public string RelativePath { get; set; }

    public string EntryPoint { get; set; }

    public string TargetProfile { get; set; }
}

public class ConfigurationAuthoringModel
{
    public int ProtocolVersion { get; set; }

    public ConfigurationFileModel Configuration { get; set; }

    public ConfigurationDiscoveryModel Discovery { get; set; }

    public ConfigurationPreviewModel Preview { get; set; }
}

public sealed class ConfigurationAuthoringResultModel : ConfigurationAuthoringModel
{
}

public sealed class ConfigurationFileModel
{
    public string Uri { get; set; }

    public bool Exists { get; set; }

    public long? ExpectedContentVersion { get; set; }

    public string ExpectedContentHash { get; set; }
}

public sealed class ConfigurationDiscoveryModel
{
    public IReadOnlyList<ConfigurationDiscoveryFileModel> Files { get; set; } =
        Array.Empty<ConfigurationDiscoveryFileModel>();

    public IReadOnlyList<string> NestedConfigurations { get; set; } =
        Array.Empty<string>();

    public int DirectoriesVisited { get; set; }

    public int CompilerProbes { get; set; }

    public bool Truncated { get; set; }

    public string TruncationReason { get; set; }

    public ConfigurationDiscoveryLimitsModel Limits { get; set; }
}

public sealed class ConfigurationDiscoveryLimitsModel
{
    public int MaxDirectories { get; set; }

    public int MaxFiles { get; set; }

    public int MaxFileSize { get; set; }

    public int MaxEntryPointsPerFile { get; set; }

    public int MaxCompilerProbes { get; set; }
}

public sealed class ConfigurationDiscoveryFileModel
{
    public string Uri { get; set; }

    public string RelativePath { get; set; }

    public long Size { get; set; }

    public IReadOnlyList<ConfigurationEntryPointModel> EntryPoints { get; set; } =
        Array.Empty<ConfigurationEntryPointModel>();

    public bool Truncated { get; set; }

    public string AnalysisError { get; set; }
}

public sealed class ConfigurationEntryPointModel
{
    public string Name { get; set; }

    public ConfigurationLocationModel Location { get; set; }

    public IReadOnlyList<string> TargetProfiles { get; set; } = Array.Empty<string>();

    public string State { get; set; }

    public string Explanation { get; set; }
}

public sealed class ConfigurationLocationModel
{
    public int Line { get; set; }

    public int Character { get; set; }
}

public sealed class ConfigurationPreviewModel
{
    public string Content { get; set; }

    public string ContentHash { get; set; }

    public bool Valid { get; set; }

    public bool Changed { get; set; }

    public IReadOnlyList<ConfigurationValidationErrorModel> Errors { get; set; } =
        Array.Empty<ConfigurationValidationErrorModel>();
}

public sealed class ConfigurationValidationErrorModel
{
    public string Code { get; set; }

    public string Field { get; set; }

    public string Message { get; set; }
}

public sealed class ConfigurationCandidateModel
{
    public ConfigurationSelectionModel Selection { get; internal set; }

    public string State { get; internal set; }

    public string Explanation { get; internal set; }

    public bool IsSelected { get; internal set; }
}

public sealed class ConfigurationSelectionListModel
{
    private readonly IReadOnlyList<ConfigurationCandidateModel> candidates;

    public ConfigurationSelectionListModel(ConfigurationAuthoringModel result)
    {
        if (result == null)
        {
            throw new ArgumentNullException(nameof(result));
        }

        candidates = (result.Discovery?.Files ?? Array.Empty<ConfigurationDiscoveryFileModel>())
            .Where(file => file != null)
            .SelectMany(file => (file.EntryPoints ?? Array.Empty<ConfigurationEntryPointModel>())
                .Where(entry => entry != null)
                .SelectMany(entry => (entry.TargetProfiles ?? Array.Empty<string>())
                    .Where(profile => !string.IsNullOrEmpty(profile))
                    .Select(profile => new ConfigurationCandidateModel
                    {
                        Selection = new ConfigurationSelectionModel
                        {
                            RelativePath = file.RelativePath,
                            EntryPoint = entry.Name,
                            TargetProfile = profile,
                        },
                        State = entry.State,
                        Explanation = entry.Explanation,
                        IsSelected = string.Equals(entry.State, "resolved", StringComparison.Ordinal) &&
                            entry.TargetProfiles?.Count == 1,
                    })))
            .ToArray();
    }

    public IReadOnlyList<ConfigurationCandidateModel> Candidates => candidates;

    public IReadOnlyList<ConfigurationSelectionModel> SelectedSelections =>
        candidates.Where(candidate => candidate.IsSelected)
            .Select(candidate => candidate.Selection)
            .ToArray();

    public void SetSelected(ConfigurationCandidateModel candidate, bool selected)
    {
        if (candidate == null || !candidates.Contains(candidate))
        {
            throw new ArgumentException("Candidate is not part of this discovery.", nameof(candidate));
        }
        candidate.IsSelected = selected;
    }
}

public static class ConfigurationPreviewValidation
{
    public static bool CanConfirm(ConfigurationAuthoringModel result) =>
        result?.Preview?.Valid == true && result.Preview.Changed;

    public static string FormatErrors(ConfigurationAuthoringModel result) =>
        string.Join(
            Environment.NewLine,
            (result?.Preview?.Errors ?? Array.Empty<ConfigurationValidationErrorModel>())
                .Where(error => error != null)
                .Select(error => $"{error.Field}: {error.Message} [{error.Code}]"));
}

public static class ConfigurationAuthoringBridge
{
    private static Func<Uri, string, long?, string, IReadOnlyList<ConfigurationSelectionModel>,
        CancellationToken, Task<ConfigurationAuthoringModel>> request;

    public static void Register(
        Func<Uri, string, long?, string, IReadOnlyList<ConfigurationSelectionModel>,
            CancellationToken, Task<ConfigurationAuthoringModel>> handler)
    {
        Volatile.Write(ref request, handler ?? throw new ArgumentNullException(nameof(handler)));
    }

    public static bool IsAvailable => Volatile.Read(ref request) != null;

    public static Task<ConfigurationAuthoringModel> RequestAsync(
        Uri workspaceFolderUri,
        string existingContent,
        long? existingVersion,
        string existingContentHash,
        IReadOnlyList<ConfigurationSelectionModel> selections,
        CancellationToken cancellationToken)
    {
        var handler = Volatile.Read(ref request);
        return handler == null
            ? Task.FromResult<ConfigurationAuthoringModel>(null)
            : handler(
                workspaceFolderUri,
                existingContent,
                existingVersion,
                existingContentHash,
                selections,
                cancellationToken);
    }
}
