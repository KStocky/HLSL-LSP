using System;
using System.Collections;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Resources;
using System.Text;
using System.Xml.Linq;
using HlslLsp.VisualStudio.Bootstrap;
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
            "OpenEffectiveConfiguration",
            "HlslContextGroup",
            "Open Effective Configuration");
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


    [Fact]
    public void VsixPackagesShaderToolsSchema()
    {
        var projectPath = Path.GetFullPath(
            Path.Combine(
                AppContext.BaseDirectory,
                @"..\..\..\..\HlslLsp.VisualStudio\HlslLsp.VisualStudio.csproj"));
        var project = XDocument.Load(projectPath);
        XNamespace ns = project.Root.Name.Namespace;
        Assert.Contains(
            project.Descendants(ns + "Content"),
            element => (string)element.Attribute("Include") == @"..\..\..\schemas\v1\shadertoolsconfig.schema.json" &&
                       (string)element.Attribute("Link") == @"schemas\v1\shadertoolsconfig.schema.json");
    }

    [Theory]
    [InlineData(0x0100, "ShowMemoryLayout", "MemoryLayout")]
    [InlineData(0x0101, "SelectVariant", "SelectVariant")]
    [InlineData(0x0102, "ShowCompilationInfo", "Compilation")]
    [InlineData(0x0103, "ShowResourceBindings", "ResourceBindings")]
    [InlineData(0x0104, "ShowPreprocessorExplorer", "PreprocessorExplorer")]
    [InlineData(0x0105, "ShowEntryPointDataFlow", "EntryPointDataFlow")]
    [InlineData(0x0106, "ShowCallHierarchy", "CallHierarchy")]
    [InlineData(0x0107, "ShowComputeVisualization", "ComputeVisualization")]
    [InlineData(0x0108, "OpenEffectiveConfiguration", "OpenEffectiveConfiguration")]
    [InlineData(0x0109, "ExpandMacro", "MacroExpansion")]
    public void RuntimeCommandIds_PreserveExistingBindings(
        int commandId,
        string commandName,
        string expected)
    {
        Assert.Equal(expected, HlslCommandIds.CommandKind(commandId).ToString());

        var commandTablePath = Path.GetFullPath(
            Path.Combine(
                AppContext.BaseDirectory,
                @"..\..\..\..\HlslLsp.VisualStudio.Bootstrap\Menus.vsct"));
        var commandTable = XDocument.Load(commandTablePath);
        XNamespace ns = "http://schemas.microsoft.com/VisualStudio/2005-10-18/CommandTable";
        var symbol = Assert.Single(
            commandTable.Descendants(ns + "IDSymbol"),
            element => (string)element.Attribute("name") == commandName);
        Assert.Equal($"0x{commandId:x4}", (string)symbol.Attribute("value"));
    }
}
