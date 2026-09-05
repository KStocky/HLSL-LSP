using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;

namespace HlslLsp.VisualStudio.Bootstrap;

public sealed class CompilationInfoModel
{
    public string EntryPoint { get; set; }

    public string Stage { get; set; }

    public string TargetProfile { get; set; }

    public string LanguageVersion { get; set; }

    public IReadOnlyList<string> Defines { get; set; } =
        Array.Empty<string>();

    public IReadOnlyList<string> CompilerArguments { get; set; } =
        Array.Empty<string>();

    public IReadOnlyList<string> IncludeDirectories { get; set; } =
        Array.Empty<string>();

    public IReadOnlyList<string> ResolvedIncludePaths { get; set; } =
        Array.Empty<string>();

    public string ActiveVariant { get; set; }

    public bool Success { get; set; }

    public IReadOnlyList<CompilationDiagnosticModel> Diagnostics { get; set; } =
        Array.Empty<CompilationDiagnosticModel>();

    public CompilationOutputModel Output { get; set; }

    public CompilationReflectionModel Reflection { get; set; }
}

public sealed class CompilationDiagnosticModel
{
    public string Severity { get; set; }

    public string Message { get; set; }

    public string Path { get; set; }

    public long Line { get; set; }

    public long Column { get; set; }
}

public sealed class CompilationOutputModel
{
    public string Type { get; set; }

    public long Size { get; set; }
}

public sealed class CompilationSignatureParameterModel
{
    public string SemanticName { get; set; }

    public long SemanticIndex { get; set; }

    public long Register { get; set; }

    public string SystemValue { get; set; }

    public string ComponentType { get; set; }

    public long Mask { get; set; }

    public long ReadWriteMask { get; set; }

    public long Stream { get; set; }
}

public sealed class CompilationResourceBindingModel
{
    public string Name { get; set; }

    public string Type { get; set; }

    public long BindPoint { get; set; }

    public long BindCount { get; set; }

    public long Space { get; set; }

    public string Dimension { get; set; }

    public string ReturnType { get; set; }
}

public sealed class CompilationThreadGroupSizeModel
{
    public long X { get; set; }

    public long Y { get; set; }

    public long Z { get; set; }
}

public sealed class CompilationReflectionModel
{
    public bool Available { get; set; }

    public string UnavailableReason { get; set; }

    public IReadOnlyList<CompilationSignatureParameterModel> InputSignature { get; set; } =
        Array.Empty<CompilationSignatureParameterModel>();

    public IReadOnlyList<CompilationSignatureParameterModel> OutputSignature { get; set; } =
        Array.Empty<CompilationSignatureParameterModel>();

    public IReadOnlyList<CompilationResourceBindingModel> Resources { get; set; } =
        Array.Empty<CompilationResourceBindingModel>();

    public CompilationThreadGroupSizeModel ThreadGroupSize { get; set; }
}

// The bridge decouples the WPF tool window (Bootstrap assembly) from the
// language client (Client assembly), mirroring MemoryLayoutBridge. There is no
// hover trigger for this feature, so only a request handler is registered;
// presentation is driven entirely by the Tools command and its refresh hooks.
public static class CompilationInfoBridge
{
    private static Func<Uri, CancellationToken, Task<CompilationInfoModel>> request;

    public static void Register(
        Func<Uri, CancellationToken, Task<CompilationInfoModel>> handler)
    {
        Volatile.Write(
            ref request,
            handler ?? throw new ArgumentNullException(nameof(handler)));
    }

    public static Task<CompilationInfoModel> RequestAsync(
        Uri uri,
        CancellationToken cancellationToken)
    {
        var handler = Volatile.Read(ref request);
        return handler == null
            ? Task.FromResult<CompilationInfoModel>(null)
            : handler(uri, cancellationToken);
    }
}
