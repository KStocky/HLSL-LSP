using HlslLsp.VisualStudio.Bootstrap;
using Xunit;

namespace HlslLsp.VisualStudio.Tests;

public sealed class ConfigurationEditGuardTests
{
    [Fact]
    public void Hash_MatchesProtocolSha256ForUtf8()
    {
        Assert.Equal(
            "sha256:2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824",
            ConfigurationEditGuard.Hash("hello"));
    }

    [Fact]
    public void IsStale_RejectsChangedBufferVersionOrContent()
    {
        var hash = ConfigurationEditGuard.Hash("{\"root\":true}");
        Assert.False(ConfigurationEditGuard.IsStale(
            true, 12, hash, true, 12, "{\"root\":true}"));
        Assert.True(ConfigurationEditGuard.IsStale(
            true, 12, hash, true, 13, "{\"root\":true}"));
        Assert.True(ConfigurationEditGuard.IsStale(
            true, 12, hash, true, 12, "{\"root\":false}"));
    }

    [Fact]
    public void IsStale_RejectsConcurrentCreationOrDeletion()
    {
        Assert.False(ConfigurationEditGuard.IsStale(
            false, null, null, false, null, ""));
        Assert.True(ConfigurationEditGuard.IsStale(
            false, null, null, true, 1, "{}"));
        Assert.True(ConfigurationEditGuard.IsStale(
            true, 1, ConfigurationEditGuard.Hash("{}"), false, null, ""));
    }
}
