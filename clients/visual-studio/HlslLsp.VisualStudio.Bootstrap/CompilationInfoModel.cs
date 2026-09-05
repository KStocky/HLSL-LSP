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

    // Present whenever compilation produced any output at all, for both
    // DXIL and SPIR-V output; SPIR-V is represented via
    // Availability == "notApplicable" rather than this being null. Null
    // only when there is no output (for example, a failed compilation).
    // See RootSignatureInfoModel.Availability for the distinct
    // absent/notApplicable/presentDetailsUnavailable/present states.
    public RootSignatureInfoModel RootSignature { get; set; }

    // Non-null whenever RootSignature is non-null: unconditionally for
    // SPIR-V output, and for DXIL output regardless of whether reflection
    // metadata itself is available. When DXIL reflection is unavailable
    // (for example, no root signature, or the reflected resource list
    // needed to compare against a present root signature could not be
    // obtained), Status reads "unknown" with an explanation rather than
    // this field being null -- reporting null compatibility while a root
    // signature is present would risk treating an unanalyzed result as
    // compatible. Both RootSignature and Compatibility are null only when
    // compilation produced no output at all (for example, a failed
    // compilation). Never treat a null value as "compatible".
    public CompilationCompatibilityModel Compatibility { get; set; }
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

// The register class a resource binds through: "cbv", "srv", "uav",
// "sampler", or "unknown". Kept as a raw string (rather than a C# enum) so
// an unrecognized future value from the server never fails deserialization.
public sealed class CompilationResourceBindingModel
{
    public string Name { get; set; }

    public string Type { get; set; }

    public long BindPoint { get; set; }

    public long BindCount { get; set; }

    public long Space { get; set; }

    public string Dimension { get; set; }

    public string ReturnType { get; set; }

    public string RegisterClass { get; set; }

    public long RawFlags { get; set; }

    public long RangeId { get; set; }

    // Reused by the compiler for structured-buffer byte stride on
    // SIT_STRUCTURED/SIT_UAV_RWSTRUCTURED* resources; 0xFFFFFFFF ("not
    // applicable") for ordinary non-multisampled textures. Passed through
    // unchanged; do not infer sample-count semantics from this value alone.
    public long SampleCount { get; set; }

    // True when BindCount == 0 (the shader-side unbounded-array sentinel).
    // Distinct from a root-signature descriptor range's own unbounded
    // convention (NumDescriptors == null); see
    // RootSignatureDescriptorRangeModel.NumDescriptors.
    public bool Unbounded { get; set; }

    public bool SystemReservedSpace { get; set; }

    // "used", "unused", or "unknown"; see ResourceUsageStatus in
    // intellisense.h for why "unused"/"unknown" are not expected in
    // practice for the shader profiles this server reflects.
    public string Usage { get; set; }

    // The declaration site of this resource in the current unsaved source
    // snapshot, or null when the compiler-reflected resource name could not
    // be matched to exactly one declaration cursor in DXC's own cursor
    // tree (absent when the name is not found at all, or when it matches
    // more than one declaration -- a genuine ambiguity the server never
    // guesses through). Never inferred client-side by searching source
    // text for the name.
    public CompilationResourceSourceLocationModel SourceLocation { get; set; }
}

// A zero-based LSP position, matching the "line"/"character" JSON keys the
// server emits for every other LSP-shaped position in this protocol.
public sealed class CompilationSourcePositionModel
{
    public long Line { get; set; }

    public long Character { get; set; }
}

public sealed class CompilationSourceRangeModel
{
    public CompilationSourcePositionModel Start { get; set; }

    public CompilationSourcePositionModel End { get; set; }
}

// The declaration site of a reflected resource, populated only when DXC's
// cursor tree identifies exactly one unambiguous declaration for it against
// the current unsaved document snapshot; see
// CompilationResourceBindingModel.SourceLocation.
public sealed class CompilationResourceSourceLocationModel
{
    public string Uri { get; set; }

    public CompilationSourceRangeModel Range { get; set; }
}

public sealed class ResourceBindingRangeModel
{
    public string ResourceName { get; set; }

    public long BaseRegister { get; set; }

    public bool Unbounded { get; set; }

    // Null when Unbounded is true; only meaningful otherwise.
    public long? EndRegister { get; set; }
}

public sealed class ResourceBindingCollisionModel
{
    public string FirstResource { get; set; }

    public string SecondResource { get; set; }

    public string RegisterClass { get; set; }

    public long Space { get; set; }

    public string Message { get; set; }
}

public sealed class ResourceBindingGroupModel
{
    public string RegisterClass { get; set; }

    public long Space { get; set; }

    public bool SystemReservedSpace { get; set; }

    public IReadOnlyList<ResourceBindingRangeModel> Ranges { get; set; } =
        Array.Empty<ResourceBindingRangeModel>();
}

// Groups are reported by the server in first-encountered order, not
// pre-sorted by space/class; presentation code must sort them (by space
// ascending, then register class in cbv/srv/uav/sampler/unknown order) to
// satisfy the space-major/class-minor grouping the Resource Bindings view
// presents.
public sealed class ResourceBindingAnalysisModel
{
    public IReadOnlyList<ResourceBindingGroupModel> Groups { get; set; } =
        Array.Empty<ResourceBindingGroupModel>();

    public IReadOnlyList<ResourceBindingCollisionModel> Collisions { get; set; } =
        Array.Empty<ResourceBindingCollisionModel>();
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

    public ResourceBindingAnalysisModel BindingAnalysis { get; set; } =
        new ResourceBindingAnalysisModel();
}

public sealed class RootSignatureDescriptorRangeModel
{
    // "srv", "uav", "cbv", "sampler", or "unknown".
    public string Type { get; set; }

    public bool Unbounded { get; set; }

    // Null when Unbounded is true (the root-signature descriptor-range
    // unbounded convention, serialized as UINT_MAX/null - distinct from the
    // shader-side BindCount == 0 convention on CompilationResourceBindingModel).
    public long? NumDescriptors { get; set; }

    public long BaseRegister { get; set; }

    public long Space { get; set; }

    public long RawFlags { get; set; }

    public long OffsetInDescriptorsFromTableStart { get; set; }
}

public sealed class RootSignatureRootConstantsModel
{
    public long ShaderRegister { get; set; }

    public long Space { get; set; }

    public long Num32BitValues { get; set; }
}

public sealed class RootSignatureRootDescriptorModel
{
    public string Type { get; set; }

    public long ShaderRegister { get; set; }

    public long Space { get; set; }

    public long RawFlags { get; set; }
}

// Exactly one of DescriptorTableRanges (non-empty), Constants, or
// RootDescriptor is populated, matching Kind
// ("descriptorTable"/"constants"/"rootDescriptor").
public sealed class RootSignatureParameterModel
{
    public string Kind { get; set; }

    // "all", "vertex", "hull", "domain", "geometry", "pixel",
    // "amplification", "mesh", or "unknown".
    public string Visibility { get; set; }

    public IReadOnlyList<RootSignatureDescriptorRangeModel> DescriptorTableRanges { get; set; } =
        Array.Empty<RootSignatureDescriptorRangeModel>();

    public RootSignatureRootConstantsModel Constants { get; set; }

    public RootSignatureRootDescriptorModel RootDescriptor { get; set; }
}

public sealed class RootSignatureStaticSamplerModel
{
    public long ShaderRegister { get; set; }

    public long Space { get; set; }

    public string Visibility { get; set; }

    public long Filter { get; set; }

    public long AddressU { get; set; }

    public long AddressV { get; set; }

    public long AddressW { get; set; }

    public double MipLodBias { get; set; }

    public long MaxAnisotropy { get; set; }

    public long ComparisonFunc { get; set; }

    public long BorderColor { get; set; }

    public double MinLod { get; set; }

    public double MaxLod { get; set; }
}

public sealed class RootSignatureDetailsModel
{
    // The signature's true, unconverted version ("1.0" or "1.1"); all
    // parameters/ranges are always exposed through the version-1.1 shape
    // regardless (flags default to NONE for a 1.0 signature).
    public string Version { get; set; }

    public long RawFlags { get; set; }

    public bool CbvSrvUavHeapDirectlyIndexed { get; set; }

    public bool SamplerHeapDirectlyIndexed { get; set; }

    public IReadOnlyList<RootSignatureParameterModel> Parameters { get; set; } =
        Array.Empty<RootSignatureParameterModel>();

    public IReadOnlyList<RootSignatureStaticSamplerModel> StaticSamplers { get; set; } =
        Array.Empty<RootSignatureStaticSamplerModel>();
}

public sealed class RootSignatureInfoModel
{
    // "present", "absent", "notApplicable", or "presentDetailsUnavailable".
    public string Availability { get; set; }

    // Populated for "notApplicable" (e.g. SPIR-V) and
    // "presentDetailsUnavailable" (e.g. non-Windows platforms), explaining
    // why. Empty otherwise.
    public string UnavailableReason { get; set; }

    // Populated only when Availability == "present".
    public RootSignatureDetailsModel Details { get; set; }
}

public sealed class ResourceCompatibilityIssueModel
{
    public string ResourceName { get; set; }

    public string RegisterClass { get; set; }

    public long Space { get; set; }

    public string Message { get; set; }
}

public sealed class CompilationCompatibilityModel
{
    // "compatible", "incompatible", or "unknown". Bindless
    // (ResourceDescriptorHeap/SamplerDescriptorHeap) accesses are invisible
    // to reflection and are never inferred; an "unknown" status must never
    // be presented as compatible.
    public string Status { get; set; }

    // Populated when Status == "unknown", explaining why compatibility
    // could not be determined.
    public string Explanation { get; set; }

    public IReadOnlyList<ResourceCompatibilityIssueModel> Issues { get; set; } =
        Array.Empty<ResourceCompatibilityIssueModel>();
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
