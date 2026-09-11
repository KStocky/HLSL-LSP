using System;
using System.Threading;
using System.Threading.Tasks;

namespace HlslLsp.VisualStudio.Bootstrap;

public sealed class MacroExpansionModel
{
    public EffectiveShaderContextModel Context { get; set; }

    public string Name { get; set; }

    public string Invocation { get; set; }

    public string Expansion { get; set; }
}

public static class MacroExpansionBridge
{
    private static Func<Uri, int, int, CancellationToken, Task<MacroExpansionModel>> request;
    private static Action<Uri, int, int> present;

    public static void Register(
        Func<Uri, int, int, CancellationToken, Task<MacroExpansionModel>> handler)
    {
        Volatile.Write(
            ref request,
            handler ?? throw new ArgumentNullException(nameof(handler)));
    }

    public static void RegisterPresenter(Action<Uri, int, int> handler)
    {
        Volatile.Write(
            ref present,
            handler ?? throw new ArgumentNullException(nameof(handler)));
    }

    public static Task<MacroExpansionModel> RequestAsync(
        Uri uri,
        int line,
        int character,
        CancellationToken cancellationToken)
    {
        var handler = Volatile.Read(ref request);
        return handler == null
            ? Task.FromResult<MacroExpansionModel>(null)
            : handler(uri, line, character, cancellationToken);
    }

    public static void Show(Uri uri, int line, int character)
        => Volatile.Read(ref present)?.Invoke(uri, line, character);
}
