using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;
using Newtonsoft.Json.Linq;

namespace HlslLsp.VisualStudio.Bootstrap;

public sealed class PreprocessorExplorerModel
{
    public string RootUri { get; set; }

    public IReadOnlyList<PreprocessorFileModel> Files { get; set; } =
        Array.Empty<PreprocessorFileModel>();

    public IReadOnlyList<PreprocessorSkippedRegionModel> SkippedRegions { get; set; } =
        Array.Empty<PreprocessorSkippedRegionModel>();

    public IReadOnlyList<PreprocessorMacroModel> Macros { get; set; } =
        Array.Empty<PreprocessorMacroModel>();

    public IReadOnlyList<PreprocessorSettingModel> Settings { get; set; } =
        Array.Empty<PreprocessorSettingModel>();

    public IReadOnlyList<string> Diagnostics { get; set; } =
        Array.Empty<string>();
}

public sealed class PreprocessorFileModel
{
    public string Uri { get; set; }

    public string LogicalPath { get; set; }

    public string PhysicalPath { get; set; }

    // "open" or "disk".
    public string Source { get; set; }

    public IReadOnlyList<PreprocessorIncludeModel> Includes { get; set; } =
        Array.Empty<PreprocessorIncludeModel>();
}

// The requested #include text and its own reported position within the
// owning file, exactly as the compiler's lexer/preprocessor state reports
// them against the current (possibly unsaved) document snapshot -- never
// derived by this client from source text.
public sealed class PreprocessorIncludeModel
{
    public string Path { get; set; }

    public long Line { get; set; }

    public long Character { get; set; }

    // "quoted", "angled", or "macro" (a macro-expanded include expression
    // whose target the compiler cannot statically resolve).
    public string Kind { get; set; }

    // "resolved", "missing", "cyclic", or "dynamic".
    public string Status { get; set; }

    // Populated only when the server was able to resolve a target file for
    // this include; null otherwise (e.g. Status == "missing" or
    // "dynamic"). Never guessed by this client from Path.
    public string ResolvedUri { get; set; }

    public string LogicalPath { get; set; }

    // Populated only when a virtual-directory-mapping prefix (see the
    // virtualDirectoryMappings entry in PreprocessorSettingModel) was used
    // to resolve this include; null otherwise.
    public string Mapping { get; set; }
}

public sealed class PreprocessorSkippedRegionModel
{
    public string Uri { get; set; }

    public CompilationSourcePositionModel Start { get; set; }

    public CompilationSourcePositionModel End { get; set; }
}

public sealed class PreprocessorMacroModel
{
    public string Name { get; set; }

    public string Value { get; set; }

    // "compiler" (defined by an actual #define the compiler evaluated) or
    // "configuration" (supplied via shadertoolsconfig.json or an editor
    // setting, with no single definition site in source text).
    public string Source { get; set; }

    public string Origin { get; set; }

    public string OriginUri { get; set; }

    // Populated only when Source == "compiler": the macro's own definition
    // site as reported by the compiler's preprocessor state. Never guessed
    // from Name when null.
    public string Uri { get; set; }

    public long? Line { get; set; }

    public long? Character { get; set; }
}

// Value mirrors whatever JSON shape the underlying setting has: a plain
// string/number/boolean for a scalar setting (languageVersion,
// targetProfile, entryPoint), a JSON array for a list setting
// (includeDirectories, additionalArguments), or a JSON object for a mapping
// setting (virtualDirectoryMappings). Kept as a raw JToken (rather than a
// strongly typed field) so the WPF presentation code can format whichever
// shape arrives without a bespoke converter per setting.
public sealed class PreprocessorSettingModel
{
    public string Name { get; set; }

    public JToken Value { get; set; }

    public string Origin { get; set; }

    public string OriginUri { get; set; }
}

// The bridge decouples the WPF tool window (Bootstrap assembly) from the
// language client (Client assembly), mirroring CompilationInfoBridge and
// MemoryLayoutBridge. There is no hover trigger for this feature, so only a
// request handler is registered; presentation is driven entirely by the
// Tools command and its refresh hooks.
public static class PreprocessorExplorerBridge
{
    private static Func<Uri, CancellationToken, Task<PreprocessorExplorerModel>> request;

    public static void Register(
        Func<Uri, CancellationToken, Task<PreprocessorExplorerModel>> handler)
    {
        Volatile.Write(
            ref request,
            handler ?? throw new ArgumentNullException(nameof(handler)));
    }

    public static Task<PreprocessorExplorerModel> RequestAsync(
        Uri uri,
        CancellationToken cancellationToken)
    {
        var handler = Volatile.Read(ref request);
        return handler == null
            ? Task.FromResult<PreprocessorExplorerModel>(null)
            : handler(uri, cancellationToken);
    }
}
