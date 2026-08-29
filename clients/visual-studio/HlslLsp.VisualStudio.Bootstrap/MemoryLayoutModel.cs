using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;

namespace HlslLsp.VisualStudio.Bootstrap;

public sealed class MemoryLayoutModel
{
    public string Name { get; set; }

    public string Type { get; set; }

    public string Mode { get; set; }

    public long Size { get; set; }

    public long Alignment { get; set; }

    public long AllocationSize { get; set; }

    public IReadOnlyList<MemoryLayoutMemberModel> Members { get; set; } =
        Array.Empty<MemoryLayoutMemberModel>();

    public IReadOnlyList<string> Diagnostics { get; set; } =
        Array.Empty<string>();
}

public sealed class MemoryLayoutMemberModel
{
    public string Name { get; set; }

    public string Type { get; set; }

    public long Offset { get; set; }

    public long Size { get; set; }

    public long Alignment { get; set; }

    public long PaddingBefore { get; set; }

    public IReadOnlyList<MemoryLayoutMemberModel> Members { get; set; } =
        Array.Empty<MemoryLayoutMemberModel>();
}

public static class MemoryLayoutBridge
{
    private static Func<Uri, int, int, CancellationToken, Task<MemoryLayoutModel>> request;
    private static Action<Uri, int, int> present;

    public static void Register(
        Func<Uri, int, int, CancellationToken, Task<MemoryLayoutModel>> handler)
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

    public static Task<MemoryLayoutModel> RequestAsync(
        Uri uri,
        int line,
        int character,
        CancellationToken cancellationToken)
    {
        var handler = Volatile.Read(ref request);
        return handler == null
            ? Task.FromResult<MemoryLayoutModel>(null)
            : handler(uri, line, character, cancellationToken);
    }

    public static void Show(Uri uri, int line, int character)
        => Volatile.Read(ref present)?.Invoke(uri, line, character);
}
