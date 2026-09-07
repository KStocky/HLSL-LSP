using System;
using System.Collections;
using System.IO;
using System.Reflection;
using System.Resources;
using System.Text;
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

    [Fact]
    public void BootstrapAssembly_ContainsComputeVisualizationToolWindow()
    {
        var assemblyPath =
            Path.Combine(AppContext.BaseDirectory, "HlslLsp.VisualStudio.Bootstrap.dll");
        var assemblyBytes = File.ReadAllBytes(assemblyPath);
        var metadata = Encoding.UTF8.GetString(assemblyBytes);

        Assert.Contains("ComputeVisualizationToolWindow", metadata);
        Assert.Contains("ShowComputeVisualizationAsync", metadata);
    }

    [Fact]
    public void CommandTable_RegistersComputeVisualizationToolsCommand()
    {
        var commandTablePath = Path.GetFullPath(
            Path.Combine(
                AppContext.BaseDirectory,
                @"..\..\..\..\HlslLsp.VisualStudio.Bootstrap\Menus.vsct"));
        var commandTable = File.ReadAllText(commandTablePath);

        Assert.Contains("id=\"ShowComputeVisualization\"", commandTable);
        Assert.Contains("<ButtonText>HLSL Compute Visualization</ButtonText>", commandTable);
        Assert.Contains("<Parent guid=\"guidHlslLspCommands\" id=\"HlslLspGroup\"/>", commandTable);
    }
}
