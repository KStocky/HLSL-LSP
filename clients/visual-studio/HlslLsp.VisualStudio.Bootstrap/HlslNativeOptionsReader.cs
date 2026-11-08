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

    internal void Load(object page)
    {
        var bindings = page.GetType().GetProperties(BindingFlags.Public | BindingFlags.Instance)
            .Select(property => new
            {
                Property = property,
                Moniker = HlslOptionsPage.GetUnifiedSettingsMoniker(property.Name),
            })
            .Where(binding => binding.Moniker != null)
            .ToArray();
        if (bindings.Length == 0)
        {
            throw new InvalidOperationException("No native settings monikers were found on the options page.");
        }
        var getValue = readerContract.GetMethod(
            "GetValueOrThrow", new[] { typeof(string), typeof(Type) });
        if (getValue == null)
        {
            throw new MissingMethodException(readerContract.FullName, "GetValueOrThrow");
        }
        // Read everything before updating the page so a failed read cannot apply a partial snapshot.
        var values = bindings.Select(binding =>
        {
            return getValue.Invoke(reader, new object[] { binding.Moniker, binding.Property.PropertyType });
        }).ToArray();
        for (var index = 0; index < bindings.Length; ++index)
        {
            bindings[index].Property.SetValue(page, values[index]);
        }
    }

    internal IDisposable Subscribe(object page, Action changed)
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
