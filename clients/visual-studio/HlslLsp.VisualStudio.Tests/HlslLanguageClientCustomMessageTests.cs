using System;
using System.Threading;
using System.Threading.Tasks;
using Nerdbank.Streams;
using StreamJsonRpc;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

// Exercises the real StreamJsonRpc wiring HlslLanguageClient relies on for
// server-owned custom notifications (hlsl/activeVariantChanged,
// hlsl/dxcRuntimeRestartRequired) and for the client-to-server
// hlsl/didChangeActiveVariant notification it sends. These run two JsonRpc
// instances over an in-memory duplex stream and drive them through the exact
// production entry points (CustomMessageTarget, AttachForCustomMessageAsync,
// UpdateActiveVariantAsync) that Visual Studio's generic LSP client uses, so
// this is concrete protocol-level evidence of correct custom-notification
// dispatch rather than an assertion that "the SDK generically supports it".
public sealed class HlslLanguageClientCustomMessageTests : IDisposable
{
    private readonly System.IO.Stream serverStream;
    private readonly System.IO.Stream clientStream;

    public HlslLanguageClientCustomMessageTests()
    {
        (serverStream, clientStream) = FullDuplexStream.CreatePair();
    }

    public void Dispose()
    {
        serverStream.Dispose();
        clientStream.Dispose();
    }

    private static async Task<bool> WaitOrTimeoutAsync(Task signal)
    {
        var completed = await Task.WhenAny(signal, Task.Delay(TimeSpan.FromSeconds(5)))
            .ConfigureAwait(false);
        return completed == signal;
    }

    [Fact]
    public async Task ServerActiveVariantChangedNotification_UpdatesClientStateAndInvokesCallback()
    {
        string observedVariant = null;
        var callbackInvoked = new TaskCompletionSource<bool>();
        var client = new HlslLanguageClient(
            "2021",
            string.Empty,
            string.Empty,
            (_, _) => Task.CompletedTask,
            variant =>
            {
                observedVariant = variant;
                callbackInvoked.TrySetResult(true);
                return Task.CompletedTask;
            });

        using var serverRpc = new JsonRpc(serverStream);
        using var clientRpc = new JsonRpc(clientStream, clientStream, client.CustomMessageTarget);
        serverRpc.StartListening();
        clientRpc.StartListening();
        await client.AttachForCustomMessageAsync(clientRpc);

        await serverRpc.NotifyWithParameterObjectAsync(
            "hlsl/activeVariantChanged",
            new { variant = "Alpha" });

        Assert.True(await WaitOrTimeoutAsync(callbackInvoked.Task));
        Assert.Equal("Alpha", observedVariant);
        Assert.Equal("Alpha", client.ActiveVariant);
    }

    [Fact]
    public async Task ServerActiveVariantChangedNotification_NullVariantClearsClientState()
    {
        string observedVariant = "not-called";
        var callbackInvoked = new TaskCompletionSource<bool>();
        var client = new HlslLanguageClient(
            "2021",
            string.Empty,
            "Alpha",
            (_, _) => Task.CompletedTask,
            variant =>
            {
                observedVariant = variant;
                callbackInvoked.TrySetResult(true);
                return Task.CompletedTask;
            });

        using var serverRpc = new JsonRpc(serverStream);
        using var clientRpc = new JsonRpc(clientStream, clientStream, client.CustomMessageTarget);
        serverRpc.StartListening();
        clientRpc.StartListening();
        await client.AttachForCustomMessageAsync(clientRpc);

        await serverRpc.NotifyWithParameterObjectAsync(
            "hlsl/activeVariantChanged",
            new { variant = (string)null });

        Assert.True(await WaitOrTimeoutAsync(callbackInvoked.Task));
        Assert.Equal(string.Empty, observedVariant);
        Assert.Equal(string.Empty, client.ActiveVariant);
    }

    [Fact]
    public async Task ClientUpdateActiveVariantAsync_SendsDidChangeActiveVariantNotificationToServer()
    {
        var client = new HlslLanguageClient(
            "2021",
            string.Empty,
            string.Empty,
            (_, _) => Task.CompletedTask,
            _ => Task.CompletedTask);

        var received = new TaskCompletionSource<string>();
        var captureTarget = new DidChangeActiveVariantCaptureTarget(received);
        using var serverRpc = new JsonRpc(serverStream, serverStream, captureTarget);
        using var clientRpc = new JsonRpc(clientStream);
        serverRpc.StartListening();
        clientRpc.StartListening();
        await client.AttachForCustomMessageAsync(clientRpc);

        await client.UpdateActiveVariantAsync("Beta");

        var completed = await Task.WhenAny(received.Task, Task.Delay(TimeSpan.FromSeconds(5)));
        Assert.Same(received.Task, completed);
        Assert.Equal("Beta", await received.Task);
    }

    [Fact]
    public async Task ServerDxcRuntimeRestartRequiredNotification_InvokesCallback()
    {
        // Regression coverage for the pre-existing notification that
        // hlsl/activeVariantChanged now reuses the same dispatch pattern from.
        string observedDirectory = null;
        string observedReason = null;
        var callbackInvoked = new TaskCompletionSource<bool>();
        var client = new HlslLanguageClient(
            "2021",
            string.Empty,
            string.Empty,
            (directory, reason) =>
            {
                observedDirectory = directory;
                observedReason = reason;
                callbackInvoked.TrySetResult(true);
                return Task.CompletedTask;
            },
            _ => Task.CompletedTask);

        using var serverRpc = new JsonRpc(serverStream);
        using var clientRpc = new JsonRpc(clientStream, clientStream, client.CustomMessageTarget);
        serverRpc.StartListening();
        clientRpc.StartListening();
        await client.AttachForCustomMessageAsync(clientRpc);

        await serverRpc.NotifyWithParameterObjectAsync(
            "hlsl/dxcRuntimeRestartRequired",
            new { directory = @"C:\runtime", reason = "configuration changed" });

        Assert.True(await WaitOrTimeoutAsync(callbackInvoked.Task));
        Assert.Equal(@"C:\runtime", observedDirectory);
        Assert.Equal("configuration changed", observedReason);
    }

    private sealed class DidChangeActiveVariantCaptureTarget
    {
        private readonly TaskCompletionSource<string> received;

        public DidChangeActiveVariantCaptureTarget(TaskCompletionSource<string> received)
        {
            this.received = received;
        }

        [JsonRpcMethod(
            "hlsl/didChangeActiveVariant",
            UseSingleObjectParameterDeserialization = true)]
        public void DidChangeActiveVariant(DidChangeActiveVariantParams parameters)
        {
            received.TrySetResult(parameters?.Variant ?? string.Empty);
        }
    }

    private sealed class DidChangeActiveVariantParams
    {
        public string Variant { get; set; }
    }
}
