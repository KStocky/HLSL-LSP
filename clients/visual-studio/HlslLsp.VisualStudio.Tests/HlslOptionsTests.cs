using System;
using System.ComponentModel;
using System.Linq;
using HlslLsp.VisualStudio.Bootstrap;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class HlslOptionsTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void UnifiedSettingsMonikersUseNativeValues(bool filterAttributes)
    {
        var provider = new HlslOptionsPage.HlslOptionsTypeDescriptionProvider(
            typeof(TestMonikerAttribute));
        var descriptor = provider.GetTypeDescriptor(typeof(HlslOptionsPage), null);
        var properties = filterAttributes
            ? descriptor.GetProperties(Array.Empty<Attribute>())
            : descriptor.GetProperties();
        var monikers = properties.Cast<PropertyDescriptor>()
            .SelectMany(property => property.Attributes.Cast<Attribute>())
            .OfType<TestMonikerAttribute>()
            .ToArray();

        Assert.Equal(12, monikers.Length);
        foreach (var attribute in monikers)
        {
            Assert.False(attribute.IsLegacyValue);
        }
        Assert.Contains(monikers, attribute =>
            attribute.Moniker == "hlslLsp.general.fileExtensions");
        Assert.Equal(monikers.Length, monikers.Select(attribute => attribute.Moniker)
            .Distinct().Count());
    }

    [Fact]
    public void UnifiedSettingsMonikersAllowOlderAttributesWithoutLegacyFlag()
    {
        var provider = new HlslOptionsPage.HlslOptionsTypeDescriptionProvider(
            typeof(OlderMonikerAttribute));
        var properties = provider.GetTypeDescriptor(typeof(HlslOptionsPage), null)
            .GetProperties();

        Assert.Equal(12, properties.Cast<PropertyDescriptor>()
            .SelectMany(property => property.Attributes.Cast<Attribute>())
            .OfType<OlderMonikerAttribute>().Count());
    }

    public sealed class TestMonikerAttribute : Attribute
    {
        public TestMonikerAttribute(string moniker)
        {
            Moniker = moniker;
        }

        public string Moniker { get; }
        public bool IsLegacyValue { get; set; } = true;
    }

    public sealed class OlderMonikerAttribute : Attribute
    {
        public OlderMonikerAttribute(string moniker)
        {
            Moniker = moniker;
        }

        public string Moniker { get; }
    }

    [Fact]
    public void NativeReaderDoesNotDependOnTypeDescriptors()
    {
        var page = new NativeTestPage();
        var provider = new EmptyInstanceProvider();
        TypeDescriptor.AddProvider(provider, typeof(NativeTestPage));
        try
        {
            Assert.Empty(TypeDescriptor.GetProperties(page).Cast<PropertyDescriptor>());
            Assert.Empty(TypeDescriptor.GetProperties(typeof(NativeTestPage)).Cast<PropertyDescriptor>());
            var storage = new NativeTestReader { FileExtensions = ".comp" };
            var reader = new HlslNativeOptionsReader(storage, typeof(NativeTestReader));
            using (reader.Subscribe(page, () => reader.Load(page)))
            {
                reader.Load(page);
                Assert.Equal(".comp", page.FileExtensions);
                Assert.Equal(new[] { "hlslLsp.general.fileExtensions", "hlslLsp.general.inlayHintTypes" }, storage.Monikers);
            }
        }
        finally
        {
            TypeDescriptor.RemoveProvider(provider, typeof(NativeTestPage));
        }
    }

    private sealed class EmptyInstanceProvider : TypeDescriptionProvider
    {
        public override ICustomTypeDescriptor GetTypeDescriptor(Type objectType, object instance)
            => new EmptyInstanceDescriptor();
    }

    private sealed class EmptyInstanceDescriptor : CustomTypeDescriptor
    {
        public override PropertyDescriptorCollection GetProperties()
            => new PropertyDescriptorCollection(Array.Empty<PropertyDescriptor>());
    }

    [Fact]
    public void NativeReaderLoadsConfiguredExtensionsBeforeActivationMatching()
    {
        var page = new NativeTestPage();
        var storage = new NativeTestReader { FileExtensions = ".comp;.vert" };
        var reader = new HlslNativeOptionsReader(storage, typeof(NativeTestReader));

        reader.Load(page);

        Assert.Equal(".comp;.vert", page.FileExtensions);
        Assert.True(page.InlayHintTypes);
        var extensions = HlslBootstrapPackage.GetActivationExtensions(page.FileExtensions);
        Assert.Contains(".comp", extensions);
        Assert.Contains(".hlsl", extensions);
        Assert.Contains(".hlsli", extensions);
    }

    [Fact]
    public void NativeReaderSubscribesToExactMonikersAndReloadsChanges()
    {
        var page = new NativeTestPage();
        var storage = new NativeTestReader();
        var reader = new HlslNativeOptionsReader(storage, typeof(NativeTestReader));
        using (reader.Subscribe(page, () => reader.Load(page)))
        {
            Assert.Equal(new[] { "hlslLsp.general.fileExtensions", "hlslLsp.general.inlayHintTypes" }, storage.Monikers);
            storage.FileExtensions = ".comp";
            storage.Changed(new NativeTestUpdate());
            Assert.Equal(".comp", page.FileExtensions);
        }
        Assert.True(storage.Disposed);
    }

    [Fact]
    public void NativeReaderDoesNotApplyPartialSnapshotWhenReadFails()
    {
        var page = new NativeTestPage();
        var storage = new NativeTestReader { FileExtensions = ".comp", FailEnabled = true };
        var reader = new HlslNativeOptionsReader(storage, typeof(NativeTestReader));

        Assert.Throws<System.Reflection.TargetInvocationException>(() => reader.Load(page));

        Assert.Equal(".hlsl;.hlsli;.usf", page.FileExtensions);
        Assert.False(page.InlayHintTypes);
    }

    public sealed class NativeTestPage
    {
        public string FileExtensions { get; set; } = ".hlsl;.hlsli;.usf";

        public bool InlayHintTypes { get; set; }
    }

    public sealed class NativeTestUpdate { }

    public sealed class NativeTestReader : IDisposable
    {
        public string FileExtensions { get; set; } = ".fx";
        public bool FailEnabled { get; set; }
        public bool Disposed { get; private set; }
        public string[] Monikers { get; private set; }
        public Action<NativeTestUpdate> Changed { get; private set; }

        public object GetValueOrThrow(string moniker, Type type)
        {
            if (moniker == "hlslLsp.general.fileExtensions")
            {
                Assert.Equal(typeof(string), type);
                return FileExtensions;
            }
            Assert.Equal("hlslLsp.general.inlayHintTypes", moniker);
            Assert.Equal(typeof(bool), type);
            if (FailEnabled)
            {
                throw new InvalidOperationException("Failed native settings read.");
            }
            return true;
        }

        public IDisposable SubscribeToChanges(Action<NativeTestUpdate> changed, string[] monikers)
        {
            Changed = changed;
            Monikers = monikers;
            return this;
        }

        public void Dispose() => Disposed = true;
    }

    [Theory]
    [InlineData(".fx", "shader.hlsl", true)]
    [InlineData(".fx", "header.hlsli", true)]
    [InlineData(".fx", "shader.HLSL", true)]
    [InlineData(".fx", "header.HLSLI", true)]
    [InlineData(".fx", "shader.fx", true)]
    [InlineData("fx", "shader.FX", true)]
    [InlineData("", "shader.hlsl", true)]
    [InlineData(null, "header.hlsli", true)]
    [InlineData(".fx", "shader.usf", false)]
    [InlineData(".fx", "notes.txt", false)]
    public void ActivationMatchesBuiltInAndConfiguredExtensions(
        string configured,
        string path,
        bool expected)
    {
        var extensions = HlslBootstrapPackage.GetActivationExtensions(configured);

        Assert.Equal(expected, extensions.Contains(System.IO.Path.GetExtension(path)));
    }
}
