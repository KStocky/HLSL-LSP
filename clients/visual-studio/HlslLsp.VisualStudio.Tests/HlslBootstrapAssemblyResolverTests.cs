using System;
using System.Globalization;
using System.IO;
using System.Reflection;
using HlslLsp.VisualStudio.Bootstrap;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class HlslBootstrapAssemblyResolverTests
{
    [Fact]
    public void ResolvesOnlyTheBootstrapAssemblyIdentity()
    {
        var bootstrap = typeof(HlslBootstrapPackage).Assembly;
        using var resolver = new HlslBootstrapAssemblyResolver(bootstrap);

        Assert.Same(
            bootstrap,
            resolver.Resolve(null, new ResolveEventArgs(bootstrap.FullName)));
        Assert.Null(resolver.Resolve(null, new ResolveEventArgs("Other.Assembly")));
    }

    [Theory]
    [InlineData(false, false)]
    [InlineData(true, true)]
    public void ResolvesClientWhenBootstrapIsLoadedOutsideTheProbingPaths(
        bool useResolver,
        bool expected)
    {
        var directory = Path.Combine(
            Path.GetTempPath(),
            "hlsl-lsp-assembly-tests",
            Guid.NewGuid().ToString("N"));
        AppDomain domain = null;
        try
        {
            var bootstrapDirectory = Path.Combine(directory, "Bootstrap");
            var clientDirectory = Path.Combine(directory, "Client");
            Directory.CreateDirectory(bootstrapDirectory);
            Directory.CreateDirectory(clientDirectory);
            var bootstrapPath = Path.Combine(
                bootstrapDirectory,
                "HlslLsp.VisualStudio.Bootstrap.dll");
            var clientPath = Path.Combine(
                clientDirectory,
                "HlslLsp.VisualStudio.dll");
            File.Copy(typeof(HlslBootstrapPackage).Assembly.Location, bootstrapPath);
            File.Copy(typeof(HlslLspActivator).Assembly.Location, clientPath);

            domain = AppDomain.CreateDomain(
                Guid.NewGuid().ToString("N"),
                null,
                new AppDomainSetup { ApplicationBase = directory });
            var probeAssembly = new Uri(
                typeof(HlslAssemblyResolutionProbe).Assembly.CodeBase).LocalPath;
            var probe = (HlslAssemblyResolutionProbe)domain.CreateInstanceFromAndUnwrap(
                probeAssembly,
                typeof(HlslAssemblyResolutionProbe).FullName);
            Assert.Equal(
                expected,
                probe.ResolveClient(
                    bootstrapPath,
                    clientPath,
                    Path.GetDirectoryName(probeAssembly),
                    useResolver));
        }
        finally
        {
            if (domain != null)
            {
                AppDomain.Unload(domain);
            }
            if (Directory.Exists(directory))
            {
                Directory.Delete(directory, true);
            }
        }
    }
}

public sealed class HlslAssemblyResolutionProbe : MarshalByRefObject
{
    public bool ResolveClient(
        string bootstrapPath,
        string clientPath,
        string dependenciesDirectory,
        bool useResolver)
    {
        var bootstrap = Assembly.LoadFile(bootstrapPath);
        var client = Assembly.LoadFrom(clientPath);
        ResolveEventHandler dependencies = (_, args) =>
        {
            var name = new AssemblyName(args.Name).Name;
            if (string.Equals(
                    name,
                    bootstrap.GetName().Name,
                    StringComparison.OrdinalIgnoreCase))
            {
                return null;
            }
            var path = Path.Combine(dependenciesDirectory, name + ".dll");
            return File.Exists(path) ? Assembly.LoadFrom(path) : null;
        };
        AppDomain.CurrentDomain.AssemblyResolve += dependencies;
        IDisposable resolver = null;
        try
        {
            if (useResolver)
            {
                var resolverType = bootstrap.GetType(
                    "HlslLsp.VisualStudio.Bootstrap.HlslBootstrapAssemblyResolver",
                    throwOnError: true);
                resolver = (IDisposable)Activator.CreateInstance(
                    resolverType,
                    BindingFlags.Instance | BindingFlags.NonPublic,
                    null,
                    new object[] { bootstrap },
                    CultureInfo.InvariantCulture);
            }
            var activate = client.GetType(
                    "HlslLsp.VisualStudio.HlslLspActivator",
                    throwOnError: true)
                .GetMethod("ActivateAsync");
            return activate.GetParameters()[0].ParameterType.Assembly == bootstrap;
        }
        catch (FileNotFoundException error) when (
            error.FileName?.StartsWith(
                bootstrap.GetName().Name + ",",
                StringComparison.OrdinalIgnoreCase) == true)
        {
            return false;
        }
        finally
        {
            resolver?.Dispose();
            AppDomain.CurrentDomain.AssemblyResolve -= dependencies;
        }
    }
}
