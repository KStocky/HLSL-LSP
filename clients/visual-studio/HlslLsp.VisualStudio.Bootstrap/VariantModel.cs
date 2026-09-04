using System;
using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;

namespace HlslLsp.VisualStudio.Bootstrap;

public sealed class VariantModel
{
    public string Name { get; set; }

    public string Description { get; set; }

    public bool Default { get; set; }

    public bool Applicable { get; set; }
}

public sealed class VariantListModel
{
    public string ActiveVariant { get; set; }

    public IReadOnlyList<VariantModel> Variants { get; set; } =
        Array.Empty<VariantModel>();
}

// Connects the Tools-menu command in the bootstrap package to the dynamically
// loaded language client, which owns the shader-variant list request and the
// active-variant notification. The client registers both handlers once it is
// activated; the command is a no-op until then.
public static class VariantBridge
{
    private static Func<Uri, CancellationToken, Task<VariantListModel>> list;
    private static Func<string, CancellationToken, Task> setActive;

    public static void Register(
        Func<Uri, CancellationToken, Task<VariantListModel>> listHandler,
        Func<string, CancellationToken, Task> setActiveHandler)
    {
        Volatile.Write(
            ref list,
            listHandler ?? throw new ArgumentNullException(nameof(listHandler)));
        Volatile.Write(
            ref setActive,
            setActiveHandler
                ?? throw new ArgumentNullException(nameof(setActiveHandler)));
    }

    public static bool IsAvailable => Volatile.Read(ref list) != null;

    public static Task<VariantListModel> ListAsync(
        Uri documentUri,
        CancellationToken cancellationToken)
    {
        var handler = Volatile.Read(ref list);
        return handler == null
            ? Task.FromResult<VariantListModel>(null)
            : handler(documentUri, cancellationToken);
    }

    public static Task SetActiveAsync(
        string variant,
        CancellationToken cancellationToken)
    {
        var handler = Volatile.Read(ref setActive);
        return handler == null
            ? Task.CompletedTask
            : handler(variant, cancellationToken);
    }
}
