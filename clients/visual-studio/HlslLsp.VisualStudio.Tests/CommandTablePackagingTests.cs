using System;
using System.Collections;
using System.IO;
using System.Reflection;
using System.Resources;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class CommandTablePackagingTests
{
    [Fact]
    public void BootstrapAssembly_EmbedsRegisteredCommandTable()
    {
        var assemblyPath =
            Path.Combine(AppContext.BaseDirectory, "HlslLsp.VisualStudio.Bootstrap.dll");
        var assembly = Assembly.ReflectionOnlyLoadFrom(assemblyPath);
        var commandTables = 0;

        foreach (var resourceName in assembly.GetManifestResourceNames())
        {
            using var stream = assembly.GetManifestResourceStream(resourceName);
            using var reader = new ResourceReader(stream);
            foreach (DictionaryEntry entry in reader)
            {
                if ((string)entry.Key == "Menus.ctmenu")
                {
                    Assert.IsType<byte[]>(entry.Value);
                    ++commandTables;
                }
            }
        }

        Assert.Equal(1, commandTables);
    }
}
