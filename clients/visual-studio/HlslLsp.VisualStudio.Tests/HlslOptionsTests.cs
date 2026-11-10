using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Linq;
using System.Reflection;
using System.Runtime.Serialization;
using HlslLsp.VisualStudio.Bootstrap;
using Microsoft.VisualStudio.Utilities.UnifiedSettings;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

[CollectionDefinition("HLSL options", DisableParallelization = true)]
public sealed class HlslOptionsCollection { }

[Collection("HLSL options")]
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
        var page = CreatePage();
        var provider = new EmptyInstanceProvider();
        TypeDescriptor.AddProvider(provider, typeof(HlslOptionsPage));
        try
        {
            Assert.Empty(TypeDescriptor.GetProperties(page).Cast<PropertyDescriptor>());
            Assert.Empty(TypeDescriptor.GetProperties(typeof(HlslOptionsPage)).Cast<PropertyDescriptor>());
            var storage = new NativeTestReader { FileExtensions = ".comp" };
            var reader = new HlslNativeOptionsReader(storage, typeof(NativeTestReader));
            using (reader.Subscribe(page, () => reader.Load(page)))
            {
                reader.Load(page);
                Assert.Equal(".comp", page.FileExtensions);
                AssertMonikers(storage.Monikers);
            }
        }
        finally
        {
            TypeDescriptor.RemoveProvider(provider, typeof(HlslOptionsPage));
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
        var page = CreatePage();
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
        var page = CreatePage();
        var storage = new NativeTestReader();
        var reader = new HlslNativeOptionsReader(storage, typeof(NativeTestReader));
        using (reader.Subscribe(page, () => reader.Load(page)))
        {
            AssertMonikers(storage.Monikers);
            storage.FileExtensions = ".comp";
            storage.Changed(new SettingsUpdate(Guid.Empty, new HashSet<string>
            {
                "hlslLsp.general.fileExtensions",
            }));
            Assert.Equal(".comp", page.FileExtensions);
        }
        Assert.True(storage.Disposed);
    }

    [Fact]
    public void NativeReaderDoesNotApplyPartialSnapshotWhenReadFails()
    {
        var page = CreatePage();
        var storage = new NativeTestReader { FileExtensions = ".comp", FailEnabled = true };
        var reader = new HlslNativeOptionsReader(storage, typeof(NativeTestReader));

        var notifications = 0;
        Action changed = () => ++notifications;
        HlslBootstrapPackage.OptionsChanged += changed;
        try
        {
            Assert.Throws<TargetInvocationException>(() => reader.Load(page));
            Assert.Equal(".hlsl;.hlsli;.usf", page.FileExtensions);
            Assert.Equal("2021", page.LanguageVersion);
            Assert.Equal("", page.CompilerBackend);
            Assert.True(page.InlayHintTypes);
            Assert.Equal(0, notifications);
        }
        finally
        {
            HlslBootstrapPackage.OptionsChanged -= changed;
        }
    }

    [Fact]
    public void NativeReaderPublishesOneCompleteSnapshotAndIgnoresUnchangedValues()
    {
        var page = CreatePage();
        var reader = new HlslNativeOptionsReader(new NativeTestReader(), typeof(NativeTestReader));
        var notifications = 0;
        Action changed = () =>
        {
            ++notifications;
            Assert.Equal(".fx", page.FileExtensions);
            Assert.Equal("202x", page.LanguageVersion);
            Assert.Equal(@"C:\dxc", page.DxcRuntimeDirectory);
            Assert.Equal("fxc", page.CompilerBackend);
            Assert.Equal(@"C:\custom\D3DCompiler.dll", page.FxcRuntimePath);
            Assert.True(page.InlayHintTypes);
            Assert.True(page.InlayHintParameters);
            Assert.True(page.InlayHintMatrixOrientation);
            Assert.True(page.InlayHintRegisters);
            Assert.True(page.InlayHintPackedOffsets);
            Assert.True(page.InlayHintArrayStrides);
            Assert.True(page.InlayHintActiveVariant);
        };
        HlslBootstrapPackage.OptionsChanged += changed;
        try
        {
            reader.Load(page);
            Assert.Equal(1, notifications);
            reader.Load(page);
            Assert.Equal(1, notifications);
        }
        finally
        {
            HlslBootstrapPackage.OptionsChanged -= changed;
        }
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void NativeInitializationCanRetryAfterReadOrSubscriptionFailure(bool subscriptionFailure)
    {
        var page = CreatePage();
        var storage = new NativeTestReader
        {
            FailEnabled = !subscriptionFailure,
            FailSubscribe = subscriptionFailure,
        };
        var reader = HlslNativeOptionsReader.FromManager(new NativeTestManager(storage));
        var notifications = 0;
        Action changed = () => ++notifications;
        HlslBootstrapPackage.OptionsChanged += changed;
        try
        {
            Assert.Throws<TargetInvocationException>(() => reader.Initialize(page, () => reader.Load(page)));
            Assert.Equal(0, storage.ActiveSubscriptions);
            Assert.Equal(".hlsl;.hlsli;.usf", page.FileExtensions);
            Assert.Equal(0, notifications);

            storage.FailEnabled = false;
            storage.FailSubscribe = false;
            using (reader.Initialize(page, () => reader.Load(page)))
            {
                Assert.Equal(1, storage.ActiveSubscriptions);
                Assert.Equal(1, notifications);
                Assert.Equal("fxc", page.CompilerBackend);
                storage.FileExtensions = ".comp";
                storage.Changed(new SettingsUpdate(Guid.Empty, new HashSet<string>
                {
                    "hlslLsp.general.fileExtensions",
                }));
                Assert.Equal(".comp", page.FileExtensions);
                Assert.Equal(2, notifications);
            }
            Assert.Equal(0, storage.ActiveSubscriptions);
        }
        finally
        {
            HlslBootstrapPackage.OptionsChanged -= changed;
        }
    }

    [Fact]
    public void MissingSettingsApisFailExplicitlyWithoutMutatingThePage()
    {
        var page = CreatePage();
        var reader = new HlslNativeOptionsReader(new object(), typeof(object));
        Assert.Throws<MissingMethodException>(() => reader.Load(page));
        Assert.Throws<MissingMethodException>(() => reader.Initialize(page, () => { }));
        Assert.Throws<InvalidOperationException>(() => HlslNativeOptionsReader.FromManager(new object()));
        Assert.Throws<InvalidOperationException>(() =>
            HlslNativeOptionsReader.FromManager(new NativeTestManager(null)));
        Assert.Equal(".hlsl;.hlsli;.usf", page.FileExtensions);
    }

    [Fact]
    public void NativeReaderUsesTheSdkManagerAndReaderContracts()
    {
        var page = CreatePage();
        var storage = new NativeTestReader();
        var reader = HlslNativeOptionsReader.FromManager(new NativeTestManager(storage));
        using (reader.Initialize(page, () => reader.Load(page)))
        {
            AssertMonikers(storage.Monikers);
            Assert.Equal(".fx", page.FileExtensions);
            Assert.Equal(@"C:\custom\D3DCompiler.dll", page.FxcRuntimePath);
        }
        Assert.True(storage.Disposed);
    }

    private static HlslOptionsPage CreatePage()
    {
        // DialogPage's constructor needs the VS host; use the real page logic without that host.
        var page = (HlslOptionsPage)FormatterServices.GetUninitializedObject(typeof(HlslOptionsPage));
        page.ApplySnapshot(new HlslOptionsSnapshot(
            ".hlsl;.hlsli;.usf", "2021", "",
            new InlayHintOptionsSnapshot(true, true, false, false, false, false, true)));
        return page;
    }

    private static void AssertMonikers(string[] monikers)
    {
        var expected = new[]
        {
            "fileExtensions", "languageVersion", "dxcRuntimeDirectory",
            "compilerBackend", "fxcRuntimePath", "inlayHintTypes", "inlayHintParameters",
            "inlayHintMatrixOrientation", "inlayHintRegisters", "inlayHintPackedOffsets",
            "inlayHintArrayStrides", "inlayHintActiveVariant",
        }.Select(name => "hlslLsp.general." + name);
        Assert.Equal(expected.OrderBy(name => name), monikers.OrderBy(name => name));
    }

    public sealed class NativeTestManager : ISettingsManager
    {
        private readonly ISettingsReader reader;

        public NativeTestManager(ISettingsReader reader) => this.reader = reader;

        public ISettingsReader GetReader() => reader;
        public ISettingsWriter GetWriter(string scope) => throw new NotSupportedException();
        public ISettingsWriter GetWriter(string scope, Guid source) => throw new NotSupportedException();
    }

    public sealed class NativeTestReader : ISettingsReader
    {
        public string FileExtensions { get; set; } = ".fx";
        public bool FailEnabled { get; set; }
        public bool FailSubscribe { get; set; }
        public bool Disposed { get; private set; }
        public int ActiveSubscriptions { get; private set; }
        public string[] Monikers { get; private set; }
        public Action<SettingsUpdate> Changed { get; private set; }

        public object GetValueOrThrow(string moniker, Type type)
        {
            if (moniker == "hlslLsp.general.fileExtensions")
            {
                Assert.Equal(typeof(string), type);
                return FileExtensions;
            }
            if (FailEnabled)
            {
                throw new InvalidOperationException("Failed native settings read.");
            }
            switch (moniker)
            {
                case "hlslLsp.general.languageVersion":
                    Assert.Equal(typeof(string), type);
                    return "202x";
                case "hlslLsp.general.dxcRuntimeDirectory":
                    Assert.Equal(typeof(string), type);
                    return @"C:\dxc";
                case "hlslLsp.general.compilerBackend":
                    Assert.Equal(typeof(string), type);
                    return "fxc";
                case "hlslLsp.general.fxcRuntimePath":
                    Assert.Equal(typeof(string), type);
                    return @"C:\custom\D3DCompiler.dll";
                default:
                    Assert.StartsWith("hlslLsp.general.inlayHint", moniker);
                    Assert.Equal(typeof(bool), type);
                    return true;
            }
        }

        public IDisposable SubscribeToChanges(Action<SettingsUpdate> changed, params string[] monikers)
        {
            if (FailSubscribe)
            {
                throw new InvalidOperationException("Failed native settings subscription.");
            }
            Changed = changed;
            Monikers = monikers;
            ++ActiveSubscriptions;
            return new NativeTestSubscription(this);
        }

        public T GetValueOrThrow<T>(string moniker) => (T)GetValueOrThrow(moniker, typeof(T));
        public IReadOnlyList<T> GetArrayOrThrow<T>(string moniker) => throw new NotSupportedException();
        public SettingRetrieval<IReadOnlyList<T>> GetArray<T>(string moniker, SettingReadOptions options) =>
            throw new NotSupportedException();
        public SettingRetrieval<T> GetValue<T>(string moniker, SettingReadOptions options) =>
            throw new NotSupportedException();
        public SettingRetrieval<object> GetValue(string moniker, Type type, SettingReadOptions options) =>
            throw new NotSupportedException();

        private sealed class NativeTestSubscription : IDisposable
        {
            private readonly NativeTestReader owner;
            private bool disposed;

            internal NativeTestSubscription(NativeTestReader owner) => this.owner = owner;

            public void Dispose()
            {
                if (!disposed)
                {
                    disposed = true;
                    owner.Disposed = true;
                    owner.Changed = null;
                    --owner.ActiveSubscriptions;
                }
            }
        }
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
