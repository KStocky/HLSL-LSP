using System;
using System.Linq;
using System.Linq.Expressions;
using System.Reflection;

namespace HlslLsp.VisualStudio.Bootstrap;

internal sealed class HlslNativeOptionsReader
{
    private readonly object reader;
    private readonly Type readerContract;

    internal HlslNativeOptionsReader(object reader, Type readerContract)
    {
        this.reader = reader;
        this.readerContract = readerContract;
    }

    internal static HlslNativeOptionsReader FromManager(object manager)
    {
        var contract = manager.GetType().GetInterfaces().FirstOrDefault(
            type => type.FullName == "Microsoft.VisualStudio.Utilities.UnifiedSettings.ISettingsManager")
            ?? throw new InvalidOperationException(
                "Visual Studio's Unified Settings manager does not expose ISettingsManager.");
        var getReader = contract.GetMethod("GetReader")
            ?? throw new MissingMethodException(contract.FullName, "GetReader");
        var reader = getReader.Invoke(manager, null)
            ?? throw new InvalidOperationException(
                "Visual Studio's Unified Settings manager did not return a settings reader.");
        return new HlslNativeOptionsReader(reader, getReader.ReturnType);
    }

    internal IDisposable Initialize(HlslOptionsPage page, Action changed)
    {
        var subscription = Subscribe(page, changed);
        try
        {
            Load(page);
            return subscription;
        }
        catch
        {
            subscription.Dispose();
            throw;
        }
    }

    internal void Load(HlslOptionsPage page)
    {
        var getValue = readerContract.GetMethod(
            "GetValueOrThrow", new[] { typeof(string), typeof(Type) });
        if (getValue == null)
        {
            throw new MissingMethodException(readerContract.FullName, "GetValueOrThrow");
        }
        T Read<T>(string propertyName)
        {
            var moniker = HlslOptionsPage.GetUnifiedSettingsMoniker(propertyName)
                ?? throw new InvalidOperationException(
                    "No native settings moniker exists for " + propertyName + ".");
            return (T)getValue.Invoke(reader, new object[] { moniker, typeof(T) });
        }

        var snapshot = new HlslOptionsSnapshot(
            Read<string>(nameof(HlslOptionsPage.FileExtensions)),
            Read<string>(nameof(HlslOptionsPage.LanguageVersion)),
            Read<string>(nameof(HlslOptionsPage.DxcRuntimeDirectory)),
            new InlayHintOptionsSnapshot(
                Read<bool>(nameof(HlslOptionsPage.InlayHintTypes)),
                Read<bool>(nameof(HlslOptionsPage.InlayHintParameters)),
                Read<bool>(nameof(HlslOptionsPage.InlayHintMatrixOrientation)),
                Read<bool>(nameof(HlslOptionsPage.InlayHintRegisters)),
                Read<bool>(nameof(HlslOptionsPage.InlayHintPackedOffsets)),
                Read<bool>(nameof(HlslOptionsPage.InlayHintArrayStrides)),
                Read<bool>(nameof(HlslOptionsPage.InlayHintActiveVariant))),
            Read<string>(nameof(HlslOptionsPage.CompilerBackend)),
            Read<string>(nameof(HlslOptionsPage.FxcRuntimePath)));

        page.ApplySnapshot(snapshot);
    }

    internal IDisposable Subscribe(HlslOptionsPage page, Action changed)
    {
        var method = readerContract.GetMethod("SubscribeToChanges");
        if (method == null)
        {
            throw new MissingMethodException(readerContract.FullName, "SubscribeToChanges");
        }
        var callbackType = method.GetParameters()[0].ParameterType;
        var update = Expression.Parameter(callbackType.GetGenericArguments()[0], "update");
        var callback = Expression.Lambda(
            callbackType,
            Expression.Invoke(Expression.Constant(changed)),
            update).Compile();
        var monikers = page.GetType().GetProperties(BindingFlags.Public | BindingFlags.Instance)
            .Select(property => HlslOptionsPage.GetUnifiedSettingsMoniker(property.Name))
            .Where(moniker => moniker != null)
            .ToArray();
        return (IDisposable)method.Invoke(reader, new object[] { callback, monikers });
    }
}
