using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.VisualStudio.LanguageServer.Client;
using Microsoft.VisualStudio.Threading;
using Newtonsoft.Json.Linq;
using StreamJsonRpc;

namespace HlslLsp.VisualStudio;

internal sealed class HlslLanguageClient :
    ILanguageClient,
    ILanguageClientCustomMessage2
{
    private Process serverProcess;
    private string languageVersion;
    private JsonRpc rpc;

    internal HlslLanguageClient(string languageVersion)
    {
        this.languageVersion = languageVersion;
    }

    public string Name => "HLSL-LSP";

    public IEnumerable<string> ConfigurationSections
    {
        get { yield return "hlsl"; }
    }

    public object InitializationOptions =>
        new
        {
            hlsl = new
            {
                languageVersion = Volatile.Read(ref languageVersion),
            },
        };

    public IEnumerable<string> FilesToWatch { get; } = Array.Empty<string>();

    public bool ShowNotificationOnInitializeFailed => true;

    public event AsyncEventHandler<EventArgs> StartAsync;

    public event AsyncEventHandler<EventArgs> StopAsync;

    public object MiddleLayer => null;

    public object CustomMessageTarget => null;

    public Task AttachForCustomMessageAsync(JsonRpc jsonRpc)
    {
        rpc = jsonRpc ?? throw new ArgumentNullException(nameof(jsonRpc));
        return Task.CompletedTask;
    }

    internal Task UpdateLanguageVersionAsync(string value)
    {
        Volatile.Write(ref languageVersion, value);
        var currentRpc = rpc;
        return currentRpc == null
            ? Task.CompletedTask
            : currentRpc.NotifyAsync(
                "hlsl/didChangeClientDefaults",
                new
                {
                    hlsl = new
                    {
                        languageVersion = value,
                    },
                });
    }

    internal async Task<JArray> GetDocumentSymbolsAsync(
        Uri documentUri,
        CancellationToken cancellationToken)
    {
        var currentRpc = rpc ??
            throw new InvalidOperationException(
                "The HLSL language server connection is unavailable.");
        for (var attempt = 0; ; ++attempt)
        {
            try
            {
                var result = await currentRpc.InvokeWithParameterObjectAsync<JToken>(
                        "textDocument/documentSymbol",
                        new
                        {
                            textDocument = new
                            {
                                uri = documentUri.AbsoluteUri,
                            },
                        },
                        cancellationToken)
                    .ConfigureAwait(false);
                return result as JArray ?? new JArray();
            }
            catch (RemoteInvocationException error)
                when (error.ErrorCode == -32602 && attempt < 20)
            {
                await Task.Delay(250, cancellationToken).ConfigureAwait(false);
            }
        }
    }

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
        var serverDirectory = Path.GetFullPath(
            Path.Combine(extensionDirectory, "..", "Server"));
        var serverPath = Path.Combine(serverDirectory, "hlsl-lsp.exe");
        if (!File.Exists(serverPath))
        {
            throw new FileNotFoundException("The bundled HLSL-LSP server was not found.", serverPath);
        }

        var startInfo = new ProcessStartInfo
        {
            FileName = serverPath,
            Arguments = "--disable-semantic-tokens",
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

    public Task OnServerInitializedAsync()
        => Task.CompletedTask;

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
        var process = Interlocked.Exchange(ref serverProcess, null);
        if (process != null)
        {
            await Task.Run(() =>
            {
                if (!process.HasExited)
                {
                    process.Kill();
                    process.WaitForExit();
                }
                process.Dispose();
            }).ConfigureAwait(false);
        }

        if (StopAsync != null)
        {
            var stop = StopAsync.InvokeAsync(this, EventArgs.Empty);
            var completed = await Task.WhenAny(stop, Task.Delay(500)).ConfigureAwait(false);
            if (completed == stop)
            {
                await stop.ConfigureAwait(false);
            }
        }
    }
}
