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
using HlslLsp.VisualStudio.Bootstrap;

namespace HlslLsp.VisualStudio;

internal sealed class HlslLanguageClient :
    ILanguageClient,
    ILanguageClientCustomMessage2
{
    private Process serverProcess;
    private string languageVersion;
    private string dxcRuntimeDirectory;
    private string activeVariant;
    private JsonRpc rpc;
    private readonly AsyncManualResetEvent rpcAttached = new();
    private readonly HlslCustomMessageTarget customMessageTarget;

    internal HlslLanguageClient(
        string languageVersion,
        string dxcRuntimeDirectory,
        string activeVariant,
        Func<string, string, Task> onRuntimeRestartRequested)
    {
        this.languageVersion = languageVersion;
        this.dxcRuntimeDirectory = dxcRuntimeDirectory ?? string.Empty;
        this.activeVariant = activeVariant ?? string.Empty;
        customMessageTarget = new HlslCustomMessageTarget(onRuntimeRestartRequested);
    }

    public string Name => "HLSL-LSP";

    public IEnumerable<string> ConfigurationSections
    {
        get { yield return "hlsl"; }
    }

    public object InitializationOptions
    {
        get
        {
            var variant = Volatile.Read(ref activeVariant);
            return new
            {
                hlsl = new
                {
                    languageVersion = Volatile.Read(ref languageVersion),
                    dxcRuntimeDirectory = Volatile.Read(ref dxcRuntimeDirectory),
                    activeVariant = string.IsNullOrEmpty(variant) ? null : variant,
                },
            };
        }
    }

    public IEnumerable<string> FilesToWatch { get; } =
        new[] { "**/shadertoolsconfig.json" };

    public bool ShowNotificationOnInitializeFailed => true;

    public event AsyncEventHandler<EventArgs> StartAsync;

    public event AsyncEventHandler<EventArgs> StopAsync;

    public object MiddleLayer => null;

    public object CustomMessageTarget => customMessageTarget;

    public Task AttachForCustomMessageAsync(JsonRpc jsonRpc)
    {
        Volatile.Write(
            ref rpc,
            jsonRpc ?? throw new ArgumentNullException(nameof(jsonRpc)));
        rpcAttached.Set();
        return Task.CompletedTask;
    }

    internal Task UpdateLanguageVersionAsync(string value)
    {
        Volatile.Write(ref languageVersion, value);
        var currentRpc = Volatile.Read(ref rpc);
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

    // The active variant is remembered so it is reapplied through
    // InitializationOptions after a runtime restart. Changing it only notifies the
    // server, which reanalyzes open documents rather than restarting.
    internal Task UpdateActiveVariantAsync(string variant)
    {
        var value = variant ?? string.Empty;
        Volatile.Write(ref activeVariant, value);
        var currentRpc = Volatile.Read(ref rpc);
        return currentRpc == null
            ? Task.CompletedTask
            : currentRpc.NotifyAsync(
                "hlsl/didChangeActiveVariant",
                new
                {
                    variant = value.Length == 0 ? null : value,
                });
    }

    internal string ActiveVariant => Volatile.Read(ref activeVariant);

    internal async Task<VariantListModel> GetVariantsAsync(
        Uri documentUri,
        CancellationToken cancellationToken)
    {
        var currentRpc = Volatile.Read(ref rpc);
        if (currentRpc == null)
        {
            await rpcAttached.WaitAsync(cancellationToken).ConfigureAwait(false);
            currentRpc = Volatile.Read(ref rpc);
            if (currentRpc == null)
            {
                throw new InvalidOperationException(
                    "The HLSL language server connection is unavailable.");
            }
        }
        var parameters = documentUri == null
            ? (object)new { }
            : new
            {
                textDocument = new
                {
                    uri = documentUri.AbsoluteUri,
                },
            };
        return await currentRpc.InvokeWithParameterObjectAsync<VariantListModel>(
                "hlsl/variants",
                parameters,
                cancellationToken)
            .ConfigureAwait(false);
    }

    internal string RuntimeDirectory => Volatile.Read(ref dxcRuntimeDirectory);

    // The DXC runtime is loaded into the server process, so changing it requires
    // recreating that process. Stopping and re-raising StartAsync makes the shell
    // call ActivateAsync again with the updated selection.
    internal async Task RestartWithRuntimeAsync(
        string languageVersion,
        string dxcRuntimeDirectory)
    {
        Volatile.Write(ref this.languageVersion, languageVersion);
        Volatile.Write(
            ref this.dxcRuntimeDirectory,
            dxcRuntimeDirectory ?? string.Empty);
        rpcAttached.Reset();
        Volatile.Write(ref rpc, null);
        await StopServerAsync().ConfigureAwait(false);
        if (StartAsync != null)
        {
            await StartAsync.InvokeAsync(this, EventArgs.Empty).ConfigureAwait(false);
        }
    }

    internal async Task<JArray> GetDocumentSymbolsAsync(
        Uri documentUri,
        CancellationToken cancellationToken)
    {
        var currentRpc = Volatile.Read(ref rpc);
        if (currentRpc == null)
        {
            await rpcAttached.WaitAsync(cancellationToken).ConfigureAwait(false);
            currentRpc = Volatile.Read(ref rpc);
            if (currentRpc == null)
            {
                throw new InvalidOperationException(
                    "The HLSL language server connection is unavailable.");
            }

        }
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

    internal async Task<MemoryLayoutModel> GetMemoryLayoutAsync(
        Uri documentUri,
        int line,
        int character,
        CancellationToken cancellationToken)
    {
        var currentRpc = Volatile.Read(ref rpc);
        if (currentRpc == null)
        {
            await rpcAttached.WaitAsync(cancellationToken).ConfigureAwait(false);
            currentRpc = Volatile.Read(ref rpc);
            if (currentRpc == null)
            {
                throw new InvalidOperationException(
                    "The HLSL language server connection is unavailable.");
            }
        }
        return await currentRpc.InvokeWithParameterObjectAsync<MemoryLayoutModel>(
                "hlsl/memoryLayout",
                new
                {
                    textDocument = new
                    {
                        uri = documentUri.AbsoluteUri,
                    },
                    position = new
                    {
                        line,
                        character,
                    },
                },
                cancellationToken)
            .ConfigureAwait(false);
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

        var arguments = "--disable-semantic-tokens";
        var runtimeDirectory = Volatile.Read(ref dxcRuntimeDirectory);
        if (!string.IsNullOrWhiteSpace(runtimeDirectory))
        {
            var resolved = ResolveRuntimeDirectory(runtimeDirectory);
            arguments += $" --dxc-runtime \"{resolved}\"";
        }

        var startInfo = new ProcessStartInfo
        {
            FileName = serverPath,
            Arguments = arguments,
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

    private static string ResolveRuntimeDirectory(string directory)
    {
        var resolved = Path.GetFullPath(directory);
        if (!Directory.Exists(resolved))
        {
            throw new DirectoryNotFoundException(
                $"The configured DXC runtime directory was not found: {resolved}");
        }

        var compiler = Path.Combine(resolved, "dxcompiler.dll");
        if (!File.Exists(compiler))
        {
            throw new FileNotFoundException(
                $"The DXC runtime directory does not contain dxcompiler.dll: {resolved}",
                compiler);
        }

        var dxil = Path.Combine(resolved, "dxil.dll");
        if (!File.Exists(dxil))
        {
            throw new FileNotFoundException(
                $"The DXC runtime directory does not contain dxil.dll: {resolved}",
                dxil);
        }

        return resolved;
    }

    public sealed class RuntimeRestartParams
    {
        public string Directory { get; set; }

        public string Reason { get; set; }
    }

    private sealed class HlslCustomMessageTarget
    {
        private readonly Func<string, string, Task> onRuntimeRestartRequested;

        public HlslCustomMessageTarget(Func<string, string, Task> handler)
        {
            onRuntimeRestartRequested = handler;
        }

        [JsonRpcMethod(
            "hlsl/dxcRuntimeRestartRequired",
            UseSingleObjectParameterDeserialization = true)]
        public Task RuntimeRestartRequiredAsync(RuntimeRestartParams parameters)
        {
            if (onRuntimeRestartRequested == null)
            {
                return Task.CompletedTask;
            }
            return onRuntimeRestartRequested(
                parameters?.Directory ?? string.Empty,
                parameters?.Reason ?? string.Empty);
        }
    }
}
