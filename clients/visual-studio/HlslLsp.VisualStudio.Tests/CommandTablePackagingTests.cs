using System;
using System.Collections;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Resources;
using System.Text;
using System.Xml.Linq;
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
    public void CommandTable_RegistersAllShaderToolsInTheHlslContextMenu()
    {
        var commandTablePath = Path.GetFullPath(
            Path.Combine(
                AppContext.BaseDirectory,
                @"..\..\..\..\HlslLsp.VisualStudio.Bootstrap\Menus.vsct"));
        var commandTable = XDocument.Load(commandTablePath);
        XNamespace ns = "http://schemas.microsoft.com/VisualStudio/2005-10-18/CommandTable";

        void AssertContextCommand(string id, string group, string label)
        {
            var button = Assert.Single(
                commandTable.Descendants(ns + "Button"),
                element => (string)element.Attribute("id") == id);
            Assert.Equal(group, (string)button.Element(ns + "Parent")?.Attribute("id"));
            Assert.Equal(
                "DynamicVisibility",
                (string)Assert.Single(button.Elements(ns + "CommandFlag")));
            Assert.Equal(label, (string)button.Descendants(ns + "ButtonText").Single());
        }

        AssertContextCommand(
            "ShowResourceBindings",
            "HlslContextInspectionGroup",
            "Resource Bindings");
        AssertContextCommand(
            "ShowPreprocessorExplorer",
            "HlslContextGroup",
            "Preprocessor Explorer");
        AssertContextCommand(
            "ShowComputeVisualization",
            "HlslContextInspectionGroup",
            "Compute Visualization");
        Assert.DoesNotContain(
            commandTable.Descendants(ns + "Parent"),
            element => (string)element.Attribute("id") == "IDM_VS_MENU_TOOLS");
    }
}
