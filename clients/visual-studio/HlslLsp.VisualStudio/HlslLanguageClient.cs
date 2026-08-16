using System;
using System.Collections.Generic;
using System.ComponentModel.Composition;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.VisualStudio.LanguageServer.Client;
using Microsoft.VisualStudio.Threading;
using Microsoft.VisualStudio.Utilities;

namespace HlslLsp.VisualStudio;

[Export(typeof(ILanguageClient))]
[ContentType(HlslContentDefinition.ContentTypeName)]
[RunOnContext(RunningContext.RunOnHost)]
public sealed class HlslLanguageClient : ILanguageClient
{
    private Process serverProcess;

    public string Name => "HLSL-LSP";

    public IEnumerable<string> ConfigurationSections
    {
        get { yield return "hlsl"; }
    }

    public object InitializationOptions => null;

    public IEnumerable<string> FilesToWatch { get; } =
        new[] { "**/*.hlsl", "**/*.hlsli", "**/shadertoolsconfig.json" };

    public bool ShowNotificationOnInitializeFailed => true;

    public event AsyncEventHandler<EventArgs> StartAsync;

    public event AsyncEventHandler<EventArgs> StopAsync;

    public async Task OnLoadedAsync()
    {
        if (StartAsync != null)
        {
            await StartAsync.InvokeAsync(this, EventArgs.Empty);
        }
    }

    public Task<Connection> ActivateAsync(CancellationToken token)
    {
        token.ThrowIfCancellationRequested();

        var extensionDirectory = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location);
        var serverDirectory = Path.Combine(extensionDirectory, "Server");
        var serverPath = Path.Combine(serverDirectory, "hlsl-lsp.exe");
        if (!File.Exists(serverPath))
        {
            throw new FileNotFoundException("The bundled HLSL-LSP server was not found.", serverPath);
        }

        var startInfo = new ProcessStartInfo
        {
            FileName = serverPath,
            WorkingDirectory = serverDirectory,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardInput = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };

        var process = new Process { StartInfo = startInfo };
        process.ErrorDataReceived += (_, eventArgs) =>
        {
            if (!string.IsNullOrEmpty(eventArgs.Data))
            {
                Debug.WriteLine($"HLSL-LSP: {eventArgs.Data}");
            }
        };
        if (!process.Start())
        {
            process.Dispose();
            throw new InvalidOperationException("Visual Studio could not start HLSL-LSP.");
        }

        serverProcess = process;
        process.BeginErrorReadLine();
        return Task.FromResult(
            new Connection(process.StandardOutput.BaseStream, process.StandardInput.BaseStream));
    }

    public Task OnServerInitializedAsync() => Task.CompletedTask;

    public Task<InitializationFailureContext> OnServerInitializeFailedAsync(
        ILanguageClientInitializationInfo initializationState)
    {
        var details = initializationState.InitializationException?.Message;
        var context = new InitializationFailureContext
        {
            FailureMessage = string.IsNullOrEmpty(details)
                ? "HLSL-LSP failed to initialize."
                : $"HLSL-LSP failed to initialize: {details}",
        };
        return Task.FromResult(context);
    }

    public async Task StopServerAsync()
    {
        if (StopAsync != null)
        {
            await StopAsync.InvokeAsync(this, EventArgs.Empty);
        }

        var process = Interlocked.Exchange(ref serverProcess, null);
        if (process == null)
        {
            return;
        }

        await Task.Run(() =>
        {
            if (!process.HasExited && !process.WaitForExit(2000))
            {
                process.Kill();
                process.WaitForExit();
            }
            process.Dispose();
        });
    }
}
