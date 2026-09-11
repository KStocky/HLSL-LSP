using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using System.Threading;
using System.Threading.Tasks;

namespace HlslLsp.VisualStudio.Bootstrap;

public sealed class EffectiveContextOriginModel
{
    public string Label { get; set; }

    public string Setting { get; set; }

    public string Uri { get; set; }
}

public sealed class EffectiveContextOriginsModel
{
    public EffectiveContextOriginModel Variant { get; set; }

    public EffectiveContextOriginModel EntryPoint { get; set; }

    public EffectiveContextOriginModel TargetProfile { get; set; }
}

public sealed class EffectiveShaderContextModel
{
    public string DocumentUri { get; set; }

    public string File { get; set; }

    public string ConfigurationUri { get; set; }

    public string ActiveVariant { get; set; }

    public string EntryPoint { get; set; }

    public string TargetProfile { get; set; }

    public EffectiveContextOriginsModel Origins { get; set; } =
        new EffectiveContextOriginsModel();
}

internal static class EffectiveShaderContextDisplay
{
    internal const string DefaultVariant = "Default";
    internal const string NotConfigured = "Not configured";

    internal static string Variant(string value)
        => string.IsNullOrEmpty(value) ? DefaultVariant : value;

    internal static string Value(string value)
        => string.IsNullOrEmpty(value) ? NotConfigured : value;

    internal static string Summary(EffectiveShaderContextModel context)
        => context == null
            ? $"File: {NotConfigured} · Variant: {DefaultVariant} · " +
              $"Entry point: {NotConfigured} · Target profile: {NotConfigured}"
            : $"File: {Value(context.File)} · Variant: {Variant(context.ActiveVariant)} · " +
              $"Entry point: {Value(context.EntryPoint)} · " +
              $"Target profile: {Value(context.TargetProfile)}";

    internal static string OriginSummary(EffectiveShaderContextModel context)
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

    internal static string ConfigurationOriginUri(EffectiveShaderContextModel context)
    {
        if (!string.IsNullOrEmpty(context?.ConfigurationUri))
        {
            return context.ConfigurationUri;
        }
        var origins = context?.Origins;
        if (!string.IsNullOrEmpty(origins?.Variant?.Uri))
        {
            return origins.Variant.Uri;
        }
        if (!string.IsNullOrEmpty(origins?.EntryPoint?.Uri))
        {
            return origins.EntryPoint.Uri;
        }
        return string.IsNullOrEmpty(origins?.TargetProfile?.Uri)
            ? null
            : origins.TargetProfile.Uri;
    }

    internal static void AddHeader(
        Panel content,
        string title,
        EffectiveShaderContextModel context)
    {
        content.Children.Add(new TextBlock
        {
            Text = title,
            FontSize = 18,
            FontWeight = FontWeights.SemiBold,
        });
        content.Children.Add(new TextBlock
        {
            Text = Summary(context),
            Margin = new Thickness(0, 3, 0, 0),
            TextWrapping = TextWrapping.Wrap,
        });
        var origins = OriginSummary(context);
        if (!string.IsNullOrEmpty(origins))
        {
            content.Children.Add(new TextBlock
            {
                Text = "Configuration origins: " + origins,
                Margin = new Thickness(0, 3, 0, 12),
                Opacity = 0.75,
                TextWrapping = TextWrapping.Wrap,
            });
        }
        else
        {
            content.Children.Add(new Border { Height = 12 });
        }
    }

    private static void AddOrigin(
        ICollection<string> values,
        string label,
        EffectiveContextOriginModel origin)
    {
        if (!string.IsNullOrEmpty(origin?.Label))
        {
            values.Add($"{label} — {origin.Label}");
        }
    }
}

public static class EffectiveShaderContextBridge
{
    private static Func<Uri, CancellationToken, Task<EffectiveShaderContextModel>> request;

    public static void Register(
        Func<Uri, CancellationToken, Task<EffectiveShaderContextModel>> handler)
    {
        Volatile.Write(
            ref request,
            handler ?? throw new ArgumentNullException(nameof(handler)));
    }

    public static Task<EffectiveShaderContextModel> RequestAsync(
        Uri uri,
        CancellationToken cancellationToken)
    {
        var handler = Volatile.Read(ref request);
        return handler == null
            ? Task.FromResult<EffectiveShaderContextModel>(null)
            : handler(uri, cancellationToken);
    }
}
