using System.Runtime.CompilerServices;

// Lets the protocol-level unit test project exercise the internal
// HlslLanguageClient / HlslCustomMessageTarget custom-notification wiring
// directly (real StreamJsonRpc dispatch over an in-memory duplex stream)
// instead of only asserting generic LSP SDK support.
[assembly: InternalsVisibleTo("HlslLsp.VisualStudio.Tests")]
