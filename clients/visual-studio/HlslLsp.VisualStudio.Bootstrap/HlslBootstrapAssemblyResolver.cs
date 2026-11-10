using System;
using System.Reflection;

namespace HlslLsp.VisualStudio.Bootstrap;

internal sealed class HlslBootstrapAssemblyResolver : IDisposable
{
    private readonly Assembly bootstrapAssembly;

    internal HlslBootstrapAssemblyResolver(Assembly bootstrapAssembly)
    {
        this.bootstrapAssembly = bootstrapAssembly ??
            throw new ArgumentNullException(nameof(bootstrapAssembly));
        AppDomain.CurrentDomain.AssemblyResolve += Resolve;
    }

    internal Assembly Resolve(object sender, ResolveEventArgs args)
        => string.Equals(
            args.Name,
            bootstrapAssembly.FullName,
            StringComparison.OrdinalIgnoreCase)
            ? bootstrapAssembly
            : null;

    public void Dispose()
    {
        AppDomain.CurrentDomain.AssemblyResolve -= Resolve;
    }
}
