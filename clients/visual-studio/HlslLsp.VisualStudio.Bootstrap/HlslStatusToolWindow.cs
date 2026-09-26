using System;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using Microsoft.VisualStudio.Shell;

namespace HlslLsp.VisualStudio.Bootstrap;

[System.Runtime.InteropServices.Guid("b92c8176-2515-4c42-a866-4973657237f8")]
public sealed class HlslStatusToolWindow : ToolWindowPane
{
    private readonly HlslStatusControl control = new();

    public HlslStatusToolWindow()
        : base(null)
    {
        Caption = "HLSL-LSP Status";
        Content = control;
        HlslStatusBridge.Changed += OnStatusChanged;
        control.SetSnapshot(HlslStatusBridge.Snapshot);
    }

    internal Uri DocumentUri { get; private set; }

    internal void ConfigureActions(
        Func<Task> restart,
        Func<Task> openOutput,
        Func<Task> openEffectiveConfiguration)
        => control.ConfigureActions(
            restart,
            openOutput,
            openEffectiveConfiguration);

    internal void BeginRefresh(Uri documentUri)
    {
        DocumentUri = documentUri;
        control.SetRefreshing(true);
    }

    internal void SetSnapshot(Uri documentUri, HlslStatusSnapshot snapshot)
    {
        DocumentUri = documentUri;
        control.SetRefreshing(false);
        control.SetSnapshot(snapshot);
    }

    private void OnStatusChanged()
    {
        HlslBootstrapPackage.RunOnMainThread(
            () => control.SetSnapshot(HlslStatusBridge.Snapshot));
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            HlslStatusBridge.Changed -= OnStatusChanged;
        }
        base.Dispose(disposing);
    }
}

internal sealed class HlslStatusControl : UserControl
{
    private readonly StackPanel content = new();
    private readonly TextBlock lifecycle = new();
    private readonly TextBlock runtime = new();
    private readonly TextBlock context = new();
    private readonly TextBlock configuration = new();
    private readonly TextBlock failure = new();
    private readonly Button restart = new() { Content = "Restart language server" };
    private readonly Button openOutput = new() { Content = "Open Output" };
    private readonly Button openConfiguration = new()
    {
        Content = "Open Effective Configuration",
    };
    private readonly Button copyDiagnostics = new() { Content = "Copy Diagnostics" };
    private HlslStatusSnapshot snapshot;
    private bool actionsConfigured;

    internal HlslStatusControl()
    {
        VisualStudioTheme.ApplyToolWindowTheme(this);
        VisualStudioTheme.ApplyButtonStyle(restart);
        VisualStudioTheme.ApplyButtonStyle(openOutput);
        VisualStudioTheme.ApplyButtonStyle(openConfiguration);
        VisualStudioTheme.ApplyButtonStyle(copyDiagnostics);

        content.Margin = new Thickness(12);
        content.Children.Add(new TextBlock
        {
            Text = "HLSL-LSP",
            FontSize = 18,
            FontWeight = FontWeights.SemiBold,
        });
        lifecycle.Margin = new Thickness(0, 4, 0, 0);
        lifecycle.FontWeight = FontWeights.SemiBold;
        content.Children.Add(lifecycle);
        runtime.Margin = new Thickness(0, 3, 0, 0);
        content.Children.Add(runtime);
        context.Margin = new Thickness(0, 10, 0, 0);
        context.TextWrapping = TextWrapping.Wrap;
        content.Children.Add(context);
        configuration.Margin = new Thickness(0, 3, 0, 0);
        configuration.Opacity = 0.75;
        configuration.TextWrapping = TextWrapping.Wrap;
        content.Children.Add(configuration);
        failure.Margin = new Thickness(0, 10, 0, 0);
        failure.FontWeight = FontWeights.SemiBold;
        failure.TextWrapping = TextWrapping.Wrap;
        content.Children.Add(failure);

        var actions = new WrapPanel { Margin = new Thickness(0, 14, 0, 0) };
        AddAction(actions, restart);
        AddAction(actions, openOutput);
        AddAction(actions, openConfiguration);
        AddAction(actions, copyDiagnostics);
        content.Children.Add(actions);
        Content = VisualStudioTheme.ApplyScrollViewerStyle(new ScrollViewer
        {
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
            Content = content,
        });

        copyDiagnostics.Click += (_, _) =>
        {
            try
            {
                Clipboard.SetText(HlslStatusDisplay.Diagnostics(snapshot));
            }
            catch (System.Runtime.InteropServices.ExternalException error)
            {
                HlslStatusBridge.ReportFailure(
                    "Could not copy HLSL-LSP diagnostics: " + error.Message);
            }
        };
    }

    internal void ConfigureActions(
        Func<Task> restartAction,
        Func<Task> openOutputAction,
        Func<Task> openConfigurationAction)
    {
        if (actionsConfigured)
        {
            return;
        }
        actionsConfigured = true;
        restart.Click += (_, _) => Run(restartAction);
        openOutput.Click += (_, _) => Run(openOutputAction);
        openConfiguration.Click += (_, _) => Run(openConfigurationAction);
    }

    internal void SetRefreshing(bool refreshing)
    {
        if (refreshing)
        {
            configuration.Text = "Refreshing active shader context…";
        }
    }

    internal void SetSnapshot(HlslStatusSnapshot value)
    {
        snapshot = value ?? new HlslStatusSnapshot();
        lifecycle.Text = HlslStatusDisplay.Lifecycle(snapshot.Lifecycle);
        runtime.Text = HlslStatusDisplay.Runtime(snapshot.Runtime);
        var shader = snapshot.Context;
        context.Text = shader == null
            ? "Active shader: Not configured"
            : $"Active shader: {HlslStatusDisplay.SafeFileName(shader.File)} · " +
              $"{EffectiveShaderContextDisplay.Variant(shader.ActiveVariant)} · " +
              $"{EffectiveShaderContextDisplay.Value(shader.EntryPoint)} · " +
              $"{EffectiveShaderContextDisplay.Value(shader.TargetProfile)}";
        var origins = HlslStatusDisplay.SafeOriginSummary(shader);
        configuration.Text = string.IsNullOrEmpty(origins)
            ? "Effective configuration: No file-backed overrides"
            : "Effective configuration: " + origins;
        var failureMessage = string.IsNullOrWhiteSpace(snapshot.LastFailure)
            ? snapshot.Runtime?.Error
            : snapshot.LastFailure;
        failure.Text = string.IsNullOrWhiteSpace(failureMessage)
            ? string.Empty
            : "Last failure: " +
              HlslStatusDisplay.RedactAbsolutePaths(failureMessage);
        failure.Visibility = string.IsNullOrWhiteSpace(failureMessage)
            ? Visibility.Collapsed
            : Visibility.Visible;
        openConfiguration.IsEnabled =
            !string.IsNullOrEmpty(
                EffectiveShaderContextDisplay.ConfigurationOriginUri(shader));
    }

    private static void AddAction(Panel panel, Button button)
    {
        button.Margin = new Thickness(0, 0, 8, 8);
        button.Padding = new Thickness(8, 3, 8, 3);
        panel.Children.Add(button);
    }

    private static void Run(Func<Task> action)
    {
        if (action != null)
        {
            HlslBootstrapPackage.RunInBackground(action, "HlslLsp/StatusAction");
        }
    }
}
