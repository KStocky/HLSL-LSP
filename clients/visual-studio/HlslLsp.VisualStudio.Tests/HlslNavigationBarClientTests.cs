using System;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.Remoting.Messaging;
using System.Runtime.Remoting.Proxies;
using System.Runtime.Serialization;
using System.Threading;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.Text;
using Microsoft.VisualStudio.TextManager.Interop;
using Newtonsoft.Json.Linq;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class HlslNavigationBarClientTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void DetachingDropdownBarDoesNotStartARefresh(bool disposed)
    {
        // Visual Studio calls SetDropdownBar(null) synchronously while removing
        // the bar, including during Dispose after the token has been disposed.
        var client = (HlslNavigationBarClient)FormatterServices.GetUninitializedObject(
            typeof(HlslNavigationBarClient));
        var cancellation = new CancellationTokenSource();
        if (disposed)
        {
            cancellation.Dispose();
        }
        typeof(HlslNavigationBarClient)
            .GetField("updateCancellation", BindingFlags.Instance | BindingFlags.NonPublic)
            .SetValue(client, cancellation);
        typeof(HlslNavigationBarClient)
            .GetField("disposed", BindingFlags.Instance | BindingFlags.NonPublic)
            .SetValue(client, disposed);

        Assert.Equal(VSConstants.S_OK, client.SetDropdownBar(null));
        if (!disposed)
        {
            cancellation.Dispose();
        }
    }

    [Fact]
    public void RebindingNeverReplacesAnotherProvidersDropdownBar()
    {
        var active = CreateClient();
        var other = CreateClient();

        Assert.True(HlslNavigationBarManager.CanRebindTo(null, active));
        Assert.True(HlslNavigationBarManager.CanRebindTo(new FakeDropdownBar(active), active));
        Assert.False(HlslNavigationBarManager.CanRebindTo(new FakeDropdownBar(other), active));
        Assert.False(HlslNavigationBarManager.CanRebindTo(new FakeDropdownBar(null), active));
    }

    [Fact]
    public void DetachingRemovesOnlyTheClientsOwnDropdownBar()
    {
        var active = CreateClient();
        var other = CreateClient();
        var owned = new FakeDropdownManager(new FakeDropdownBar(active));
        var foreign = new FakeDropdownManager(new FakeDropdownBar(other));
        var empty = new FakeDropdownManager(null);

        HlslNavigationBarClient.RemoveOwnedDropdownBar(owned, active);
        HlslNavigationBarClient.RemoveOwnedDropdownBar(foreign, active);
        HlslNavigationBarClient.RemoveOwnedDropdownBar(empty, active);

        Assert.Equal(1, owned.Removals);
        Assert.Equal(0, foreign.Removals);
        Assert.Equal(0, empty.Removals);
    }

    [Fact]
    public void OwnershipTransitionsPreserveTheNativeDropdown()
    {
        var active = CreateClient();
        var nativeBar = new FakeDropdownBar(CreateClient());
        var manager = new FakeDropdownManager(new FakeDropdownBar(active));
        HlslNavigationBarClient.RemoveOwnedDropdownBar(manager, active);
        Assert.Null(manager.Bar);

        manager.Bar = nativeBar;
        Assert.False(HlslNavigationBarManager.CanRebindTo(manager.Bar, active));
        HlslNavigationBarClient.RemoveOwnedDropdownBar(manager, active);
        Assert.Same(nativeBar, manager.Bar);
        Assert.Equal(1, manager.Removals);

        manager.Bar = null;
        Assert.True(HlslNavigationBarManager.CanRebindTo(manager.Bar, active));
        manager.Bar = new FakeDropdownBar(active);
        HlslNavigationBarClient.RemoveOwnedDropdownBar(manager, active);
        Assert.Null(manager.Bar);
        Assert.Equal(2, manager.Removals);
    }

    [Fact]
    public void CaretInsideBodiesSelectsNestedScopesAndTheCorrectOverload()
    {
        var symbols = new JArray(
            Symbol("Outer", 23, 1, 15,
                Symbol("Inner", 23, 3, 9, Symbol("member", 6, 4, 8))),
            Symbol("work", 12, 20, 23),
            Symbol("work", 12, 25, 29));
        var client = CreateClient();
        var scopes = typeof(HlslNavigationBarClient)
            .GetMethod("CreateScopes", BindingFlags.Static | BindingFlags.NonPublic)
            .Invoke(null, new object[] { symbols });
        SetField(client, "scopes", scopes);
        SetField(client, "buffer", CreateBuffer());
        var bar = new FakeDropdownBar(client);
        SetField(client, "dropdownBar", bar);

        client.RefreshSelectionsAt(600);
        Assert.Equal(2, bar.Selections[0]);
        Assert.Equal(0, bar.Selections[1]);
        Assert.Equal(VSConstants.S_OK, client.GetEntryText(0, 2, out var scope));
        Assert.Equal("Outer::Inner", scope);
        client.GetEntryText(1, 0, out var member);
        Assert.Equal("member", member);

        client.RefreshSelectionsAt(2100);
        Assert.Equal(0, bar.Selections[0]);
        Assert.Equal(0, bar.Selections[1]);
        client.RefreshSelectionsAt(2600);
        Assert.Equal(0, bar.Selections[0]);
        Assert.Equal(1, bar.Selections[1]);
        client.GetComboAttributes(1, out var count, out _, out _);
        Assert.Equal(2u, count);

        client.RefreshSelectionsAt(3500);
        Assert.Equal(0, bar.Selections[0]);
        Assert.Equal(-1, bar.Selections[1]);
    }

    private static JObject Symbol(string name, int kind, int start, int end, params JObject[] children)
    {
        JObject Position(int line, int character) => new()
        {
            ["line"] = line,
            ["character"] = character,
        };
        return new JObject
        {
            ["name"] = name,
            ["kind"] = kind,
            ["range"] = new JObject { ["start"] = Position(start, 0), ["end"] = Position(end, 80) },
            ["selectionRange"] = new JObject { ["start"] = Position(start, 5), ["end"] = Position(start, 10) },
            ["children"] = new JArray(children),
        };
    }

    private static void SetField(object target, string name, object value) =>
        target.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)
            .SetValue(target, value);

    private static ITextBuffer CreateBuffer()
    {
        ITextSnapshot snapshot = null;
        snapshot = new TextProxy<ITextSnapshot>(call =>
        {
            switch (call.MethodName)
            {
                case "get_Length":
                    return 4000;
                case "get_LineCount":
                    return 40;
                case "GetLineFromLineNumber":
                    var start = (int)call.Args[0] * 100;
                    return new TextProxy<ITextSnapshotLine>(lineCall =>
                        lineCall.MethodName switch
                        {
                            "get_Start" => new SnapshotPoint(snapshot, start),
                            "get_End" => new SnapshotPoint(snapshot, start + 80),
                            _ => throw new NotSupportedException(lineCall.MethodName),
                        }).Value;
                default:
                    throw new NotSupportedException(call.MethodName);
            }
        }).Value;
        return new TextProxy<ITextBuffer>(call => call.MethodName == "get_CurrentSnapshot"
            ? snapshot
            : throw new NotSupportedException(call.MethodName)).Value;
    }

    private sealed class TextProxy<T> : RealProxy
    {
        private readonly Func<IMethodCallMessage, object> invoke;

        internal TextProxy(Func<IMethodCallMessage, object> invoke) : base(typeof(T)) =>
            this.invoke = invoke;

        internal T Value => (T)GetTransparentProxy();

        public override IMessage Invoke(IMessage message)
        {
            var call = (IMethodCallMessage)message;
            return new ReturnMessage(invoke(call), null, 0, call.LogicalCallContext, call);
        }
    }

    private static HlslNavigationBarClient CreateClient() =>
        (HlslNavigationBarClient)FormatterServices.GetUninitializedObject(
            typeof(HlslNavigationBarClient));

    private sealed class FakeDropdownBar : IVsDropdownBar
    {
        private readonly IVsDropdownBarClient owner;

        internal FakeDropdownBar(IVsDropdownBarClient owner) => this.owner = owner;

        internal Dictionary<int, int> Selections { get; } = new();

        public int GetClient(out IVsDropdownBarClient client)
        {
            client = owner;
            return VSConstants.S_OK;
        }

        public int Attach(IntPtr window, int combos, IVsDropdownBarClient client) =>
            throw new NotSupportedException();

        public void Detach() => throw new NotSupportedException();

        public int GetCurrentSelection(int combo, out int selection)
        {
            Selections.TryGetValue(combo, out selection);
            return VSConstants.S_OK;
        }

        public int RefreshCombo(int combo, int selection)
        {
            Selections[combo] = selection;
            return VSConstants.S_OK;
        }

        public int SetCurrentSelection(int combo, int selection) =>
            throw new NotSupportedException();
    }

    private sealed class FakeDropdownManager : IVsDropdownBarManager
    {
        internal IVsDropdownBar Bar { get; set; }

        internal FakeDropdownManager(IVsDropdownBar bar) => Bar = bar;

        internal int Removals { get; private set; }

        public int GetDropdownBar(out IVsDropdownBar value)
        {
            value = Bar;
            return VSConstants.S_OK;
        }

        public int RemoveDropdownBar()
        {
            ++Removals;
            Bar = null;
            return VSConstants.S_OK;
        }

        public int AddDropdownBar(int combos, IVsDropdownBarClient client) =>
            throw new NotSupportedException();
    }
}
