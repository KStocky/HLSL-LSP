using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Media;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.Shell;
using Microsoft.VisualStudio.Shell.Interop;
using Microsoft.VisualStudio.TextManager.Interop;

namespace HlslLsp.VisualStudio.Bootstrap;

// A dedicated **HLSL Resource Bindings** tool window: register-space/class
// grouping, collisions, embedded root-signature state, and root-signature
// compatibility. Kept separate from CompilationInfoToolWindow (which shows a
// flat resource list alongside configuration/diagnostics/output) so neither
// window becomes an unreadable single page. Both windows request the same
// hlsl/compilationInfo RPC against the same current (possibly unsaved)
// document via the existing CompilationInfoBridge; no new bridge/RPC is
// introduced.
[Guid("73727df0-3059-4eca-b7e8-23b64d3586f2")]
public sealed class ResourceBindingsToolWindow : ToolWindowPane
{
    private readonly ResourceBindingsControl control = new();

    public ResourceBindingsToolWindow()
        : base(null)
    {
        Caption = "HLSL Resource Bindings";
        Content = control;
    }

    // Tracks the document the window currently shows so external refresh
    // triggers (active-variant selection, document save) know which document
    // to re-request without needing the caret or active-view context.
    internal Uri DocumentUri { get; private set; }

    internal void SetInfo(Uri uri, CompilationInfoModel info)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        DocumentUri = uri;
        control.SetInfo(info);
    }

    // A failed or cancelled request must never regress this window to the
    // "open a document" placeholder or leave it stuck showing a previous
    // request's transient state: SetError keeps whatever the control
    // already renders when it has real content, and only replaces the
    // placeholder with an explicit error when there is no content yet.
    internal void SetError(Uri uri, string message, bool preserveSameDocumentContent)
    {
        var preserveContent =
            preserveSameDocumentContent &&
            DocumentUri != null &&
            DocumentUri.Equals(uri);
        DocumentUri = uri;
        control.SetError(message, preserveContent);
    }
}

internal sealed class ResourceBindingsControl : UserControl
{
    private static readonly string[] RegisterClassOrder = { "cbv", "srv", "uav", "sampler", "unknown" };
    private static readonly IReadOnlyDictionary<string, string> RegisterClassLabels =
        new Dictionary<string, string>
        {
            ["cbv"] = "CBV",
            ["srv"] = "SRV",
            ["uav"] = "UAV",
            ["sampler"] = "Sampler",
            ["unknown"] = "Unknown",
        };

    private readonly StackPanel content = new();
    private bool hasContent;

    internal ResourceBindingsControl()
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        Content = new ScrollViewer
        {
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
            Content = content,
        };
        SetInfo(null);
    }

    internal void SetInfo(CompilationInfoModel info)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        hasContent = info != null;
        content.Children.Clear();
        content.Margin = new Thickness(12);
        if (info == null)
        {
            content.Children.Add(new TextBlock
            {
                Text = "Open an HLSL document, then run " +
                       "Tools > HLSL Resource Bindings.",
                TextWrapping = TextWrapping.Wrap,
            });
            return;
        }

        AddHeader(info);
        AddSection("Resources", () => AddResources(info.Reflection));
        AddSection("Collisions", () => AddCollisions(info.Reflection));
        AddSection("Root signature", () => AddRootSignature(info.RootSignature));
        AddSection("Compatibility", () => AddCompatibility(info.Compatibility));
    }

    internal void SetError(string message, bool preserveContent)
    {
        if (hasContent && preserveContent)
        {
            // Keep the last successful content on screen rather than
            // replacing it with an error for a transient refresh failure.
            return;
        }
        hasContent = false;
        content.Children.Clear();
        content.Margin = new Thickness(12);
        content.Children.Add(new TextBlock
        {
            Text = message,
            Foreground = Brushes.OrangeRed,
            TextWrapping = TextWrapping.Wrap,
        });
    }

    private void AddHeader(CompilationInfoModel info)
    {
        var title = string.IsNullOrEmpty(info.EntryPoint)
            ? info.TargetProfile
            : $"{info.EntryPoint} ({info.TargetProfile})";
        content.Children.Add(new TextBlock
        {
            Text = "Resource Bindings: " +
                   (string.IsNullOrEmpty(title) ? "(default entry point)" : title),
            FontSize = 18,
            FontWeight = FontWeights.SemiBold,
        });
        var variant = string.IsNullOrEmpty(info.ActiveVariant)
            ? "(none)"
            : info.ActiveVariant;
        content.Children.Add(new TextBlock
        {
            Text = $"Stage {info.Stage} · active variant {variant}",
            Margin = new Thickness(0, 3, 0, 6),
            Opacity = 0.75,
        });
        if (!info.Success)
        {
            content.Children.Add(new TextBlock
            {
                Text = "Compilation failed; resource bindings reflect the last " +
                       "successfully compiled output, if any. See HLSL Shader " +
                       "Compilation for diagnostics.",
                Foreground = Brushes.Goldenrod,
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 0, 0, 6),
            });
        }
        content.Children.Add(new TextBlock
        {
            Text = "Resource names and collision participants are clickable only " +
                   "when DXC's reflection supplies an unambiguous declaration " +
                   "location for them; a resource whose name cannot be found, or " +
                   "that matches more than one declaration, renders as plain " +
                   "text instead of a guessed link. Embedded root-signature " +
                   "entries have no declaration location in this protocol, so " +
                   "they are never clickable.",
            TextWrapping = TextWrapping.Wrap,
            Opacity = 0.75,
            Margin = new Thickness(0, 0, 0, 12),
        });
    }

    private void AddSection(string title, Action addBody)
    {
        content.Children.Add(new TextBlock
        {
            Text = title,
            FontSize = 14,
            FontWeight = FontWeights.SemiBold,
            Margin = new Thickness(0, 14, 0, 6),
        });
        addBody();
    }

    // --- Resources ---------------------------------------------------

    private void AddResources(CompilationReflectionModel reflection)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (reflection == null)
        {
            AddMuted("Resource bindings are not available because no compiled output was produced.");
            return;
        }
        if (!reflection.Available)
        {
            AddWarning(
                "Resource bindings are unavailable: " +
                (string.IsNullOrEmpty(reflection.UnavailableReason)
                    ? "unknown reason"
                    : reflection.UnavailableReason));
            return;
        }
        var groups = SortedGroups(reflection.BindingAnalysis?.Groups);
        if (groups.Count == 0)
        {
            AddMuted("The compiled shader has no bound resources.");
            return;
        }
        AddMuted(
            "Grouped by register space, then register class. System-reserved " +
            "spaces (0xfffffff0\u20130xffffffff) are compiler/driver-internal and " +
            "are excluded from collision detection.");
        var lookup = ResourceLookup(reflection.Resources);
        foreach (var group in groups)
        {
            var reserved = group.SystemReservedSpace ? "  [system-reserved]" : string.Empty;
            content.Children.Add(new TextBlock
            {
                Text = $"Space {group.Space} \u2014 " +
                       $"{Label(group.RegisterClass)}{reserved}",
                FontWeight = FontWeights.SemiBold,
                Foreground = group.SystemReservedSpace ? Brushes.Goldenrod : null,
                Margin = new Thickness(0, 8, 0, 4),
            });
            AddResourceRangeTable(group, lookup);
        }
    }

    private static IReadOnlyList<ResourceBindingGroupModel> SortedGroups(
        IReadOnlyList<ResourceBindingGroupModel> groups)
    {
        if (groups == null || groups.Count == 0)
        {
            return Array.Empty<ResourceBindingGroupModel>();
        }
        return groups
            .OrderBy(group => group.Space)
            .ThenBy(group => RegisterClassRank(group.RegisterClass))
            .ToList();
    }

    private static int RegisterClassRank(string registerClass)
    {
        var index = Array.IndexOf(RegisterClassOrder, registerClass ?? "unknown");
        return index < 0 ? RegisterClassOrder.Length : index;
    }

    private static string Label(string registerClass)
        => RegisterClassLabels.TryGetValue(registerClass ?? "unknown", out var label)
            ? label
            : registerClass ?? "Unknown";

    // Full per-resource metadata (type/dimension/return type/sample
    // count/usage) lives in reflection.Resources, while grouping/range data
    // lives in reflection.BindingAnalysis. Both are computed by the server
    // from the same reflected register data, so a
    // (registerClass, space, name) key reliably joins them for display.
    private static string ResourceKey(string registerClass, long space, string name)
        => $"{registerClass}|{space.ToString(CultureInfo.InvariantCulture)}|{name}";

    private static IReadOnlyDictionary<string, CompilationResourceBindingModel> ResourceLookup(
        IReadOnlyList<CompilationResourceBindingModel> resources)
    {
        // Excludes a key entirely (rather than keeping either candidate)
        // when more than one resource shares the same
        // (registerClass, space, name) key -- this should not happen for
        // valid reflection data, but silently picking one candidate could
        // point a collision participant or resource-row link at the wrong
        // declaration. Dictionary<TKey,TValue>.TryAdd is unavailable on
        // net472, so ambiguity is detected with an explicit ContainsKey
        // check instead.
        var map = new Dictionary<string, CompilationResourceBindingModel>();
        var ambiguousKeys = new HashSet<string>();
        foreach (var resource in resources ?? Array.Empty<CompilationResourceBindingModel>())
        {
            var key = ResourceKey(resource.RegisterClass, resource.Space, resource.Name);
            if (map.ContainsKey(key))
            {
                ambiguousKeys.Add(key);
                continue;
            }
            map[key] = resource;
        }
        foreach (var key in ambiguousKeys)
        {
            map.Remove(key);
        }
        return map;
    }

    private static string RangeText(ResourceBindingRangeModel range)
        => range.Unbounded
            ? $"{range.BaseRegister} and above (unbounded)"
            : range.EndRegister == range.BaseRegister
                ? range.BaseRegister.ToString(CultureInfo.InvariantCulture)
                : $"{range.BaseRegister}-{range.EndRegister}";

    private static string ArrayText(CompilationResourceBindingModel resource)
    {
        if (resource == null)
        {
            return "-";
        }
        if (resource.Unbounded)
        {
            return "unbounded";
        }
        return resource.BindCount == 1 ? "scalar" : $"[{resource.BindCount}]";
    }

    // Resource types whose reflected SampleCount field is repurposed by the
    // compiler to report the structured-buffer element byte stride rather
    // than a real multisample count. Mirrors dxc::resource_type_name's exact
    // server strings for D3D_SIT_STRUCTURED and every UAV structured-buffer
    // variant (RWStructuredBuffer, RWStructuredBuffer with an implicit
    // hidden counter, AppendStructuredBuffer, ConsumeStructuredBuffer): all
    // of these declare a struct stride, so all of them reuse NumSamples the
    // same way.
    private static readonly HashSet<string> StructuredBufferTypes = new(StringComparer.Ordinal)
    {
        "structured_buffer",
        "uav_rwstructured",
        "uav_rwstructured_with_counter",
        "uav_append_structured",
        "uav_consume_structured",
    };

    private static string SampleCountText(CompilationResourceBindingModel resource)
    {
        if (resource == null)
        {
            return "-";
        }
        if (resource.Type != null && StructuredBufferTypes.Contains(resource.Type))
        {
            return $"stride {resource.SampleCount} bytes";
        }
        return resource.SampleCount == 0xffffffff ? "n/a" : resource.SampleCount.ToString(CultureInfo.InvariantCulture);
    }

    private static string OrDash(string value) => string.IsNullOrEmpty(value) ? "-" : value;

    private void AddResourceRangeTable(
        ResourceBindingGroupModel group,
        IReadOnlyDictionary<string, CompilationResourceBindingModel> lookup)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        var grid = new Grid();
        foreach (var width in new[] { 150d, 130d, 80d, 110d, 100d, 100d, 130d, 80d })
        {
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(width) });
        }
        var row = 0;
        AddTableRow(
            grid,
            row++,
            new[]
            {
                "Name", "Register range", "Array", "Type", "Dimension", "Return type",
                "Sample count / stride", "Usage",
            },
            true);
        foreach (var range in group.Ranges ?? Array.Empty<ResourceBindingRangeModel>())
        {
            lookup.TryGetValue(
                ResourceKey(group.RegisterClass, group.Space, range.ResourceName),
                out var resource);
            AddResourceTableDataRow(
                grid,
                row++,
                range.ResourceName,
                resource?.SourceLocation,
                new[]
                {
                    RangeText(range),
                    ArrayText(resource),
                    resource?.Type ?? "-",
                    OrDash(resource?.Dimension),
                    OrDash(resource?.ReturnType),
                    SampleCountText(resource),
                    resource?.Usage ?? "unknown",
                });
        }
        content.Children.Add(grid);
    }

    // Renders a data row whose Name cell (column 0) is a clickable
    // Hyperlink only when `location` is a compiler-supplied, unambiguous
    // declaration site for this exact resource -- never guessed from
    // `name`. The remaining columns render as plain text via the same
    // layout AddTableRow uses.
    private void AddResourceTableDataRow(
        Grid grid,
        int row,
        string name,
        CompilationResourceSourceLocationModel location,
        string[] remainingValues)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        var nameCell = CreateResourceNameInline(name, location);
        Grid.SetRow(nameCell, row);
        Grid.SetColumn(nameCell, 0);
        grid.Children.Add(nameCell);
        for (var column = 0; column < remainingValues.Length; ++column)
        {
            var text = new TextBlock
            {
                Text = remainingValues[column] ?? string.Empty,
                TextTrimming = TextTrimming.CharacterEllipsis,
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(5, 4, 5, 4),
            };
            Grid.SetRow(text, row);
            Grid.SetColumn(text, column + 1);
            grid.Children.Add(text);
        }
    }

    private TextBlock CreateResourceNameInline(string name, CompilationResourceSourceLocationModel location)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        var block = new TextBlock
        {
            TextTrimming = TextTrimming.CharacterEllipsis,
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(5, 4, 5, 4),
        };
        if (location == null)
        {
            block.Inlines.Add(new Run(name ?? string.Empty));
            return block;
        }
        var link = new Hyperlink(new Run(name ?? string.Empty))
        {
            ToolTip = "Go to declaration",
        };
        link.Click += (_, _) => NavigateToResourceLocation(location);
        block.Inlines.Add(link);
        return block;
    }

    // --- Collisions ----------------------------------------------------

    private void AddCollisions(CompilationReflectionModel reflection)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        if (reflection == null || !reflection.Available)
        {
            AddMuted("Collisions cannot be determined because resource bindings are unavailable.");
            return;
        }
        var collisions = reflection.BindingAnalysis?.Collisions ?? Array.Empty<ResourceBindingCollisionModel>();
        if (collisions.Count == 0)
        {
            AddMuted("No provable register-range collisions were found between distinct resources.");
            return;
        }
        // Collision participants are resolved only through the exact
        // resource entries in this same response (matched by
        // registerClass+space+name, the same key AddResourceRangeTable uses
        // to join range data back to a resource) -- never by searching
        // source text for the name.
        var lookup = ResourceLookup(reflection.Resources);
        foreach (var collision in collisions)
        {
            var line = new TextBlock
            {
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(0, 0, 0, 4),
            };
            line.Inlines.Add(new Run(
                $"{Label(collision.RegisterClass)} space {collision.Space}: {collision.Message} ("));
            AppendCollisionParticipant(line, collision.RegisterClass, collision.Space, collision.FirstResource, lookup);
            line.Inlines.Add(new Run(" \u2194 "));
            AppendCollisionParticipant(line, collision.RegisterClass, collision.Space, collision.SecondResource, lookup);
            line.Inlines.Add(new Run(")"));
            content.Children.Add(line);
        }
    }

    private void AppendCollisionParticipant(
        TextBlock line,
        string registerClass,
        long space,
        string name,
        IReadOnlyDictionary<string, CompilationResourceBindingModel> lookup)
    {
        ThreadHelper.ThrowIfNotOnUIThread();
        lookup.TryGetValue(ResourceKey(registerClass, space, name), out var resource);
        if (resource?.SourceLocation != null)
        {
            var link = new Hyperlink(new Run(name ?? string.Empty))
            {
                ToolTip = "Go to declaration",
            };
            var location = resource.SourceLocation;
            link.Click += (_, _) => NavigateToResourceLocation(location);
            line.Inlines.Add(link);
        }
        else
        {
            line.Inlines.Add(new Run(name ?? string.Empty));
        }
    }

    // --- Root signature --------------------------------------------------

    private void AddRootSignature(RootSignatureInfoModel rootSignature)
    {
        if (rootSignature == null)
        {
            AddMuted("Root-signature information is not available because compilation did not produce any output (for example, a failed compilation).");
            return;
        }
        switch (rootSignature.Availability)
        {
            case "absent":
                AddMuted(
                    "No embedded root signature was found in this compiled output " +
                    "(for example, no [RootSignature(...)] attribute or matching " +
                    "compiler argument).");
                return;
            case "notApplicable":
                AddMuted(
                    string.IsNullOrEmpty(rootSignature.UnavailableReason)
                        ? "Root signatures do not apply to this compilation target."
                        : rootSignature.UnavailableReason);
                return;
            case "presentDetailsUnavailable":
                AddWarning(
                    "An embedded root signature is present, but its details could " +
                    "not be retrieved on this platform: " +
                    (string.IsNullOrEmpty(rootSignature.UnavailableReason)
                        ? "unknown reason"
                        : rootSignature.UnavailableReason));
                return;
            case "present":
                if (rootSignature.Details == null)
                {
                    AddWarning("An embedded root signature is reported present, but no details were captured.");
                    return;
                }
                AddRootSignatureDetails(rootSignature.Details);
                return;
            default:
                AddMuted("Root-signature availability is unrecognized.");
                return;
        }
    }

    private void AddRootSignatureDetails(RootSignatureDetailsModel details)
    {
        AddKeyValue("Version", details.Version);
        AddKeyValue("Flags", "0x" + details.RawFlags.ToString("x", CultureInfo.InvariantCulture));
        AddKeyValue("CBV/SRV/UAV heap directly indexed", details.CbvSrvUavHeapDirectlyIndexed ? "yes" : "no");
        AddKeyValue("Sampler heap directly indexed", details.SamplerHeapDirectlyIndexed ? "yes" : "no");

        content.Children.Add(new TextBlock
        {
            Text = "Root parameters",
            FontWeight = FontWeights.SemiBold,
            Margin = new Thickness(0, 8, 0, 4),
        });
        var parameters = details.Parameters ?? Array.Empty<RootSignatureParameterModel>();
        if (parameters.Count == 0)
        {
            AddMuted("(no root parameters)");
        }
        else
        {
            for (var index = 0; index < parameters.Count; ++index)
            {
                AddRootSignatureParameter(parameters[index], index);
            }
        }

        content.Children.Add(new TextBlock
        {
            Text = "Static samplers",
            FontWeight = FontWeights.SemiBold,
            Margin = new Thickness(0, 8, 0, 4),
        });
        var staticSamplers = details.StaticSamplers ?? Array.Empty<RootSignatureStaticSamplerModel>();
        if (staticSamplers.Count == 0)
        {
            AddMuted("(no static samplers)");
        }
        else
        {
            AddStaticSamplerTable(staticSamplers);
        }
    }

    private void AddRootSignatureParameter(RootSignatureParameterModel parameter, int index)
    {
        content.Children.Add(new TextBlock
        {
            Text = $"Parameter {index} \u2014 {parameter.Kind} (visibility: {parameter.Visibility})",
            FontWeight = FontWeights.SemiBold,
            Opacity = 0.85,
            Margin = new Thickness(0, 6, 0, 2),
        });
        switch (parameter.Kind)
        {
            case "descriptorTable":
                AddDescriptorRangeTable(
                    parameter.DescriptorTableRanges ?? Array.Empty<RootSignatureDescriptorRangeModel>());
                break;
            case "constants" when parameter.Constants != null:
                content.Children.Add(new TextBlock
                {
                    Text = $"root constants: register b{parameter.Constants.ShaderRegister}, " +
                           $"space {parameter.Constants.Space}, " +
                           $"{parameter.Constants.Num32BitValues} x 32-bit values",
                    TextWrapping = TextWrapping.Wrap,
                });
                break;
            case "rootDescriptor" when parameter.RootDescriptor != null:
                content.Children.Add(new TextBlock
                {
                    Text = $"root descriptor: {Label(parameter.RootDescriptor.Type).ToUpperInvariant()} " +
                           $"register {parameter.RootDescriptor.ShaderRegister}, " +
                           $"space {parameter.RootDescriptor.Space}, " +
                           $"flags 0x{parameter.RootDescriptor.RawFlags:x}",
                    TextWrapping = TextWrapping.Wrap,
                });
                break;
        }
    }

    private void AddDescriptorRangeTable(IReadOnlyList<RootSignatureDescriptorRangeModel> ranges)
    {
        var grid = new Grid();
        foreach (var width in new[] { 80d, 100d, 70d, 90d, 80d, 80d })
        {
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(width) });
        }
        var row = 0;
        AddTableRow(
            grid,
            row++,
            new[] { "Type", "Base register", "Space", "Count", "Offset", "Flags" },
            true);
        foreach (var range in ranges)
        {
            var count = range.Unbounded ? "unbounded" : range.NumDescriptors?.ToString(CultureInfo.InvariantCulture) ?? "0";
            AddTableRow(
                grid,
                row++,
                new[]
                {
                    Label(range.Type).ToUpperInvariant(),
                    range.BaseRegister.ToString(CultureInfo.InvariantCulture),
                    range.Space.ToString(CultureInfo.InvariantCulture),
                    count,
                    range.OffsetInDescriptorsFromTableStart.ToString(CultureInfo.InvariantCulture),
                    "0x" + range.RawFlags.ToString("x", CultureInfo.InvariantCulture),
                },
                false);
        }
        content.Children.Add(grid);
    }

    private void AddStaticSamplerTable(IReadOnlyList<RootSignatureStaticSamplerModel> samplers)
    {
        var grid = new Grid();
        foreach (var width in new[] { 80d, 70d, 90d, 70d, 110d, 110d })
        {
            grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(width) });
        }
        var row = 0;
        AddTableRow(
            grid,
            row++,
            new[] { "Register", "Space", "Visibility", "Filter", "Address U/V/W", "LOD range" },
            true);
        foreach (var sampler in samplers)
        {
            AddTableRow(
                grid,
                row++,
                new[]
                {
                    sampler.ShaderRegister.ToString(CultureInfo.InvariantCulture),
                    sampler.Space.ToString(CultureInfo.InvariantCulture),
                    sampler.Visibility,
                    "0x" + sampler.Filter.ToString("x", CultureInfo.InvariantCulture),
                    $"{sampler.AddressU}/{sampler.AddressV}/{sampler.AddressW}",
                    $"{sampler.MinLod}\u2013{sampler.MaxLod}",
                },
                false);
        }
        content.Children.Add(grid);
    }

    // --- Compatibility --------------------------------------------------

    private void AddCompatibility(CompilationCompatibilityModel compatibility)
    {
        if (compatibility == null)
        {
            AddMuted(
                "Compatibility information is not available because compilation did not " +
                "produce any output (for example, a failed compilation).");
            AddBindlessNote();
            return;
        }
        switch (compatibility.Status)
        {
            case "unknown":
                AddWarning(
                    "Compatibility is unknown: " +
                    (string.IsNullOrEmpty(compatibility.Explanation)
                        ? "unknown reason"
                        : compatibility.Explanation) +
                    ". Unavailable root-signature details are never treated as compatible.");
                break;
            case "incompatible":
                content.Children.Add(new TextBlock
                {
                    Text = "Incompatible",
                    Foreground = Brushes.OrangeRed,
                    FontWeight = FontWeights.SemiBold,
                    Margin = new Thickness(0, 0, 0, 4),
                });
                foreach (var issue in compatibility.Issues ?? Array.Empty<ResourceCompatibilityIssueModel>())
                {
                    content.Children.Add(new TextBlock
                    {
                        Text = $"{issue.ResourceName} ({Label(issue.RegisterClass)}, space {issue.Space}): {issue.Message}",
                        TextWrapping = TextWrapping.Wrap,
                        Margin = new Thickness(0, 0, 0, 4),
                    });
                }
                break;
            case "compatible":
                content.Children.Add(new TextBlock
                {
                    Text = "Compatible",
                    Foreground = Brushes.LimeGreen,
                    FontWeight = FontWeights.SemiBold,
                    Margin = new Thickness(0, 0, 0, 4),
                });
                content.Children.Add(new TextBlock
                {
                    Text = "All reflected, user-addressable resources are covered by " +
                           "the embedded root signature for the compiled entry point's stage.",
                    TextWrapping = TextWrapping.Wrap,
                });
                break;
            default:
                AddMuted("Compatibility status is unrecognized.");
                break;
        }
        AddBindlessNote();
    }

    private void AddBindlessNote()
    {
        AddMuted(
            "Bindless accesses through ResourceDescriptorHeap/SamplerDescriptorHeap " +
            "are invisible to compiler reflection and can never be enumerated or " +
            "checked here; this analysis only covers resources DXC reflected as " +
            "explicit bound-resource declarations.");
    }

    // --- Shared helpers --------------------------------------------------

    private void AddMuted(string text)
    {
        content.Children.Add(new TextBlock
        {
            Text = text,
            TextWrapping = TextWrapping.Wrap,
            Opacity = 0.75,
            Margin = new Thickness(0, 0, 0, 4),
        });
    }

    private void AddWarning(string text)
    {
        content.Children.Add(new TextBlock
        {
            Text = text,
            TextWrapping = TextWrapping.Wrap,
            Foreground = Brushes.Goldenrod,
            Margin = new Thickness(0, 0, 0, 4),
        });
    }

    private static void AddTableRow(Grid grid, int row, string[] values, bool header)
    {
        grid.RowDefinitions.Add(new RowDefinition { Height = GridLength.Auto });
        for (var column = 0; column < values.Length; ++column)
        {
            var text = new TextBlock
            {
                Text = values[column] ?? string.Empty,
                FontWeight = header ? FontWeights.SemiBold : FontWeights.Normal,
                TextTrimming = TextTrimming.CharacterEllipsis,
                TextWrapping = TextWrapping.Wrap,
                Margin = new Thickness(5, 4, 5, 4),
            };
            Grid.SetRow(text, row);
            Grid.SetColumn(text, column);
            grid.Children.Add(text);
        }
    }

    private void AddKeyValue(string key, string value)
    {
        var line = new TextBlock
        {
            TextWrapping = TextWrapping.Wrap,
            Margin = new Thickness(0, 0, 0, 4),
        };
        line.Inlines.Add(new System.Windows.Documents.Run($"{key}: ")
        {
            FontWeight = FontWeights.SemiBold,
        });
        line.Inlines.Add(new System.Windows.Documents.Run(value));
        content.Children.Add(line);
    }

    // --- Navigation --------------------------------------------------

    // Opens the authoritative file/range on an explicit user click (never
    // from background save/variant refresh, which never calls this method).
    // Validates the untrusted uri/range before touching any file or VS SDK
    // API, and surfaces an explicit error dialog on any failure rather than
    // failing silently or guessing a fallback location.
    private void NavigateToResourceLocation(CompilationResourceSourceLocationModel location)
    {
        // Hyperlink.Click always fires on the UI thread, but the VS SDK's
        // threading analyzer still requires this explicit assertion before
        // touching IVsWindowFrame/IVsTextView.
        ThreadHelper.ThrowIfNotOnUIThread();
        if (!TryValidateLocation(
                location,
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
            ShowNavigationError($"Unable to navigate to the resource declaration: {ex.Message}");
        }
    }

    private static void ShowNavigationError(string message)
    {
        VsShellUtilities.ShowMessageBox(
            ServiceProvider.GlobalProvider,
            message,
            "HLSL Resource Bindings",
            OLEMSGICON.OLEMSGICON_WARNING,
            OLEMSGBUTTON.OLEMSGBUTTON_OK,
            OLEMSGDEFBUTTON.OLEMSGDEFBUTTON_FIRST);
    }

    // Validates an untrusted CompilationResourceSourceLocationModel before
    // any file-system or VS SDK API call touches it: the uri must be an
    // absolute local-file URI whose target exists, and the range must be a
    // well-formed, non-inverted set of non-negative integer positions that
    // fit in the Int32 range the VS text-view APIs require.
    private static bool TryValidateLocation(
        CompilationResourceSourceLocationModel location,
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
        if (location == null || string.IsNullOrEmpty(location.Uri))
        {
            error = "The resource location was missing.";
            return false;
        }
        Uri uri;
        try
        {
            uri = new Uri(location.Uri, UriKind.Absolute);
        }
        catch (UriFormatException)
        {
            error = "The resource location's URI could not be parsed.";
            return false;
        }
        if (!uri.IsFile)
        {
            error = "The resource location does not refer to a local file.";
            return false;
        }
        var range = location.Range;
        if (range?.Start == null || range.End == null)
        {
            error = "The resource location's range was missing.";
            return false;
        }
        if (!TryToInt32(range.Start.Line, out startLine) ||
            !TryToInt32(range.Start.Character, out startCharacter) ||
            !TryToInt32(range.End.Line, out endLine) ||
            !TryToInt32(range.End.Character, out endCharacter))
        {
            error = "The resource location's range contained an invalid position.";
            return false;
        }
        if (endLine < startLine || (endLine == startLine && endCharacter < startCharacter))
        {
            error = "The resource location's range was inverted.";
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
