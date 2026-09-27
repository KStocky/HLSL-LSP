using System;
using System.Security.Cryptography;
using System.Text;

namespace HlslLsp.VisualStudio.Bootstrap;

internal static class ConfigurationEditGuard
{
    internal static string Hash(string content)
    {
        using var sha = SHA256.Create();
        var bytes = sha.ComputeHash(Encoding.UTF8.GetBytes(content));
        var hex = new StringBuilder("sha256:");
        foreach (var value in bytes)
        {
            hex.Append(value.ToString("x2"));
        }
        return hex.ToString();
    }

    internal static bool IsStale(
        bool expectedExists,
        long? expectedVersion,
        string expectedHash,
        bool exists,
        long? currentVersion,
        string currentContent)
        => expectedExists != exists ||
           (expectedVersion.HasValue &&
            expectedVersion != currentVersion) ||
           (expectedHash != null &&
            !string.Equals(
                expectedHash,
                Hash(currentContent),
                StringComparison.Ordinal));
}
