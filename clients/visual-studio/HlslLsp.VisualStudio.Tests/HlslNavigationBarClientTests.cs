using System;
using System.Reflection;
using System.Runtime.Serialization;
using System.Threading;
using Microsoft.VisualStudio;
using Microsoft.VisualStudio.TextManager.Interop;
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

    private static HlslNavigationBarClient CreateClient() =>
        (HlslNavigationBarClient)FormatterServices.GetUninitializedObject(
            typeof(HlslNavigationBarClient));

    private sealed class FakeDropdownBar : IVsDropdownBar
    {
        private readonly IVsDropdownBarClient owner;

        internal FakeDropdownBar(IVsDropdownBarClient owner) => this.owner = owner;

        public int GetClient(out IVsDropdownBarClient client)
        {
            client = owner;
            return VSConstants.S_OK;
        }

        public int Attach(IntPtr window, int combos, IVsDropdownBarClient client) =>
            throw new NotSupportedException();

        public void Detach() => throw new NotSupportedException();

        public int GetCurrentSelection(int combo, out int selection) =>
            throw new NotSupportedException();

        public int RefreshCombo(int combo, int selection) =>
            throw new NotSupportedException();

        public int SetCurrentSelection(int combo, int selection) =>
            throw new NotSupportedException();
    }

    private sealed class FakeDropdownManager : IVsDropdownBarManager
    {
        private readonly IVsDropdownBar bar;

        internal FakeDropdownManager(IVsDropdownBar bar) => this.bar = bar;

        internal int Removals { get; private set; }

        public int GetDropdownBar(out IVsDropdownBar value)
        {
            value = bar;
            return VSConstants.S_OK;
        }

        public int RemoveDropdownBar()
        {
            ++Removals;
            return VSConstants.S_OK;
        }

        public int AddDropdownBar(int combos, IVsDropdownBarClient client) =>
            throw new NotSupportedException();
    }
}
